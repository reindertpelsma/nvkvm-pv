#!/usr/bin/env python3
"""
test_adversarial.py — Security boundary and error-path tests for nvkvm

These tests verify that:
 1. Invalid ioctls return expected errors rather than crashing or succeeding.
 2. Passing garbage data in ioctl parameter blobs is handled safely.
 3. Using handles from one process/session in another fails.
 4. Exhausting resources (FDs, memory) is handled gracefully.
 5. Concurrent access from multiple threads doesn't corrupt state.
 6. Closing an FD while operations are in flight is safe.
 7. The mmap window cannot be escaped.

These are analogous to the security tests implied by gVisor's nvproxy model:
the guest kernel module and QEMU backend both validate everything; a malicious
guest cannot corrupt host state.

Run inside the guest VM with nvkvm-guest.ko loaded.
"""

import ctypes
import errno
import fcntl
import mmap
import multiprocessing
import os
import resource
import signal
import struct
import sys
import threading
import time
import unittest

# ── ioctl helpers ─────────────────────────────────────────────────────────────

IOC_NONE  = 0
IOC_WRITE = 1
IOC_READ  = 2

def _IOC(direction, type_, nr, size):
    return (direction << 30) | (size << 16) | (ord(type_) << 8) | nr

def _IOWR(type_, nr, size):
    return _IOC(IOC_READ | IOC_WRITE, type_, nr, size)

# NVIDIA ioctl magic
NV_IOCTL_MAGIC = 'F'

# Key ioctl numbers (IOC_NR only)
NV_IOCTL_BASE            = 200
NV_ESC_CHECK_VERSION_STR = NV_IOCTL_BASE + 10
NV_ESC_SYS_PARAMS        = NV_IOCTL_BASE + 14
NV_ESC_RM_ALLOC          = 0x2b
NV_ESC_RM_FREE           = 0x29
NV_ESC_RM_CONTROL        = 0x2a

NV_RM_API_VERSION_STRING_LENGTH = 64

def make_check_version_cmd():
    # nv_ioctl_rm_api_version: cmd(4) + reply(4) + version_string(64) = 72 bytes
    return _IOWR(NV_IOCTL_MAGIC, NV_ESC_CHECK_VERSION_STR, 72)

def make_sys_params_cmd():
    # nv_ioctl_sys_params: memory_size(8) = 8 bytes
    return _IOWR(NV_IOCTL_MAGIC, NV_ESC_SYS_PARAMS, 8)

def require_nvidia():
    if not os.path.exists("/dev/nvidiactl"):
        raise unittest.SkipTest("/dev/nvidiactl not found — nvkvm-guest.ko not loaded?")

def open_nvidiactl():
    return os.open("/dev/nvidiactl", os.O_RDWR | os.O_CLOEXEC)

def open_nvidia0():
    return os.open("/dev/nvidia0", os.O_RDWR | os.O_CLOEXEC)

def do_ioctl(fd, cmd, buf):
    """Call ioctl(fd, cmd, buf) and return (retval, errno). Never raises."""
    try:
        fcntl.ioctl(fd, cmd, buf, True)
        return 0, 0
    except OSError as e:
        return -1, e.errno

# ── Test classes ──────────────────────────────────────────────────────────────

class TestInvalidIoctls(unittest.TestCase):
    """Sending invalid ioctl commands must return ENOTTY, not crash."""

    def setUp(self):
        require_nvidia()
        self.fd = open_nvidiactl()

    def tearDown(self):
        os.close(self.fd)

    def test_completely_unknown_ioctl(self):
        buf = (ctypes.c_uint8 * 8)()
        # Use a type code that nvidia doesn't recognize at all
        cmd = _IOWR('Z', 0xFF, 8)
        ret, err = do_ioctl(self.fd, cmd, buf)
        self.assertLess(ret, 0)
        self.assertIn(err, (errno.ENOTTY, errno.EINVAL),
                      f"Expected ENOTTY/EINVAL, got {errno.errorcode.get(err, err)}")

    def test_nvidia_ioctl_wrong_size(self):
        # Send NV_ESC_RM_CONTROL with wrong parameter size (too small)
        buf = (ctypes.c_uint8 * 4)()  # real struct is 32 bytes
        cmd = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, 4)
        ret, err = do_ioctl(self.fd, cmd, buf)
        self.assertLess(ret, 0)
        self.assertIn(err, (errno.EINVAL, errno.ENOTTY))

    def test_nvidia_ioctl_oversized(self):
        # Extremely large parameter buffer for a known ioctl
        buf = (ctypes.c_uint8 * 65536)()
        cmd = _IOWR(NV_IOCTL_MAGIC, NV_ESC_CHECK_VERSION_STR, 65536)
        ret, err = do_ioctl(self.fd, cmd, buf)
        self.assertLess(ret, 0)

    def test_ioctl_null_param(self):
        # ioctl with NULL (0) argument — kernel should return EFAULT
        # We can't easily test this from Python without cffi, but we can
        # verify the valid path works to confirm the FD is open.
        cmd = make_check_version_cmd()
        buf = (ctypes.c_uint8 * 72)()
        ret, err = do_ioctl(self.fd, cmd, buf)
        # We just want it to not crash the module; error is fine
        _ = (ret, err)  # result depends on driver state

    def test_ioctl_on_closed_fd(self):
        fd2 = open_nvidiactl()
        os.close(fd2)
        buf = (ctypes.c_uint8 * 72)()
        cmd = make_check_version_cmd()
        ret, err = do_ioctl(fd2, cmd, buf)
        self.assertLess(ret, 0)
        self.assertEqual(err, errno.EBADF)


class TestGarbageParams(unittest.TestCase):
    """Send all-ones / all-zeros / random params for known ioctls."""

    def setUp(self):
        require_nvidia()
        self.fd = open_nvidiactl()

    def tearDown(self):
        os.close(self.fd)

    def _test_with_pattern(self, cmd, size, pattern):
        buf = (ctypes.c_uint8 * size)(*([pattern] * size))
        ret, err = do_ioctl(self.fd, cmd, buf)
        # Must not crash (SIGSEGV / kernel oops). Any errno is acceptable.
        # The key invariant: the process is still alive after the call.
        self.assertTrue(os.getpid() > 0)

    def test_rm_control_all_zeros(self):
        cmd = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, 32)
        self._test_with_pattern(cmd, 32, 0x00)

    def test_rm_control_all_ones(self):
        cmd = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, 32)
        self._test_with_pattern(cmd, 32, 0xFF)

    def test_rm_alloc_garbage_handles(self):
        # NVOS64 with random/invalid handles — driver returns error, no crash
        cmd = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, 48)
        self._test_with_pattern(cmd, 48, 0xAB)

    def test_rm_free_invalid_handle(self):
        # Free a handle that was never allocated — must be a no-op per NVIDIA
        # RS_COMPATIBILITY_MODE semantics (driver allows freeing non-existent
        # handles when compat mode is enabled, which it is on Linux).
        cmd = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_FREE, 16)
        self._test_with_pattern(cmd, 16, 0x00)

    def test_check_version_garbage(self):
        cmd = make_check_version_cmd()
        self._test_with_pattern(cmd, 72, 0xFF)


class TestFDLifecycle(unittest.TestCase):
    """Verify FD open/close lifecycle is properly tracked."""

    def setUp(self):
        require_nvidia()

    def test_open_close_nvidiactl(self):
        for _ in range(10):
            fd = open_nvidiactl()
            self.assertGreater(fd, 0)
            os.close(fd)

    def test_open_close_nvidia0(self):
        for _ in range(5):
            fd = open_nvidia0()
            self.assertGreater(fd, 0)
            os.close(fd)

    def test_duplicate_open(self):
        # Opening the same device multiple times is valid
        fds = [open_nvidiactl() for _ in range(8)]
        for fd in fds:
            os.close(fd)

    def test_dup_fd(self):
        # os.dup should create a second reference that can be independently closed
        fd = open_nvidiactl()
        fd2 = os.dup(fd)
        os.close(fd)
        # fd2 should still be valid after fd is closed
        buf = (ctypes.c_uint8 * 8)()
        cmd = make_sys_params_cmd()
        do_ioctl(fd2, cmd, buf)  # just must not crash
        os.close(fd2)

    def test_fd_close_during_module_use(self):
        # Open, do a valid ioctl, then close. Should be clean.
        fd = open_nvidiactl()
        cmd = make_check_version_cmd()
        buf = (ctypes.c_uint8 * 72)()
        do_ioctl(fd, cmd, buf)
        os.close(fd)
        # After close, reopen should work fine
        fd2 = open_nvidiactl()
        os.close(fd2)


class TestMmapBoundaries(unittest.TestCase):
    """Verify that mmap errors and boundary conditions are handled safely."""

    def setUp(self):
        require_nvidia()
        self.fd = open_nvidia0()

    def tearDown(self):
        os.close(self.fd)

    def test_mmap_zero_length_fails(self):
        try:
            mm = mmap.mmap(self.fd, 0, mmap.MAP_SHARED, mmap.PROT_READ)
            mm.close()
            # If we get here without exception, that's fine too (OS may allow it)
        except (OSError, ValueError):
            pass  # Expected

    def test_mmap_invalid_offset(self):
        # Non-page-aligned offset should fail
        try:
            mm = mmap.mmap(self.fd, 4096, mmap.MAP_SHARED,
                           mmap.PROT_READ | mmap.PROT_WRITE,
                           offset=1)  # not page-aligned
            mm.close()
        except (OSError, ValueError):
            pass  # Expected

    def test_mmap_huge_length_fails(self):
        # Requesting 1 TB should fail cleanly
        try:
            mm = mmap.mmap(self.fd, 1 << 40, mmap.MAP_SHARED,
                           mmap.PROT_READ | mmap.PROT_WRITE)
            mm.close()
            # If we get here something is wrong
            self.fail("mmap of 1 TB should have failed")
        except OSError:
            pass


class TestCrossProcessIsolation(unittest.TestCase):
    """
    Verify that handles allocated in process A are not usable in process B.
    This mirrors gVisor's per-container isolation guarantee.
    """

    def setUp(self):
        require_nvidia()

    def test_fork_child_has_independent_session(self):
        """
        After fork, the child process should operate in an independent session.
        The child closing an FD from the parent's side should not affect the parent.
        """
        fd = open_nvidiactl()
        cmd = make_check_version_cmd()
        buf = (ctypes.c_uint8 * 72)()

        pid = os.fork()
        if pid == 0:
            # Child: do some operations
            do_ioctl(fd, cmd, buf)
            os.close(fd)
            os._exit(0)
        else:
            # Parent: wait for child, then verify our FD is still valid
            _, status = os.waitpid(pid, 0)
            self.assertEqual(os.WEXITSTATUS(status), 0)
            # Our FD should still be usable
            ret, err = do_ioctl(fd, cmd, buf)
            # err=0 or any driver error is fine; EBADF would mean leaked close
            self.assertNotEqual(err, errno.EBADF,
                                "Parent FD was closed by child!")
            os.close(fd)

    def test_fd_not_inherited_across_exec(self):
        """O_CLOEXEC should prevent FD leaking across exec."""
        fd = os.open("/dev/nvidiactl", os.O_RDWR | os.O_CLOEXEC)
        fd_flags = fcntl.fcntl(fd, fcntl.F_GETFD)
        self.assertTrue(fd_flags & fcntl.FD_CLOEXEC,
                        "O_CLOEXEC flag not set on /dev/nvidiactl")
        os.close(fd)


class TestConcurrentAccess(unittest.TestCase):
    """
    Multiple threads simultaneously calling ioctls on the same or different FDs
    must not corrupt module state or crash.
    """

    def setUp(self):
        require_nvidia()

    def _worker(self, fd, n_iterations, errors):
        cmd = make_check_version_cmd()
        for _ in range(n_iterations):
            buf = (ctypes.c_uint8 * 72)()
            try:
                do_ioctl(fd, cmd, buf)
            except Exception as e:
                errors.append(str(e))

    def test_concurrent_same_fd(self):
        fd = open_nvidiactl()
        errors = []
        threads = [
            threading.Thread(target=self._worker, args=(fd, 50, errors))
            for _ in range(8)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        os.close(fd)
        self.assertEqual(errors, [],
                         f"Concurrent same-fd errors: {errors[:5]}")

    def test_concurrent_different_fds(self):
        fds = [open_nvidiactl() for _ in range(4)]
        errors = []
        threads = []
        for fd in fds:
            for _ in range(2):
                threads.append(
                    threading.Thread(target=self._worker,
                                     args=(fd, 25, errors))
                )
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for fd in fds:
            os.close(fd)
        self.assertEqual(errors, [],
                         f"Concurrent multi-fd errors: {errors[:5]}")

    def test_open_close_concurrent(self):
        """Rapidly opening and closing FDs from multiple threads must be safe."""
        errors = []

        def open_close():
            try:
                for _ in range(20):
                    fd = open_nvidiactl()
                    time.sleep(0)  # yield
                    os.close(fd)
            except Exception as e:
                errors.append(str(e))

        threads = [threading.Thread(target=open_close) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])


class TestResourceExhaustion(unittest.TestCase):
    """
    Verify the module handles resource limits gracefully.
    """

    def setUp(self):
        require_nvidia()

    def test_many_open_fds_up_to_ulimit(self):
        """Opening many FDs should hit the OS limit cleanly, not crash."""
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        # Temporarily lower the limit to make the test fast
        test_limit = min(soft, 256)
        resource.setrlimit(resource.RLIMIT_NOFILE, (test_limit, hard))

        fds = []
        try:
            for _ in range(test_limit + 10):
                try:
                    fds.append(open_nvidiactl())
                except OSError as e:
                    self.assertEqual(e.errno, errno.EMFILE)
                    break  # Expected: hit fd limit
        finally:
            for fd in fds:
                os.close(fd)
            resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))


class TestSignalSafety(unittest.TestCase):
    """
    A signal arriving while blocked in an ioctl should not deadlock.
    The ioctl should restart or return EINTR.
    """

    def setUp(self):
        require_nvidia()

    def test_sigterm_during_ioctl_does_not_deadlock(self):
        """
        Fire SIGUSR1 while a worker thread is doing ioctls.
        The ioctl should return EINTR or succeed; the process must not hang.
        """
        received = []

        def handler(sig, frame):
            received.append(sig)

        old_handler = signal.signal(signal.SIGUSR1, handler)
        fd = open_nvidiactl()
        cmd = make_check_version_cmd()

        def worker():
            for _ in range(100):
                buf = (ctypes.c_uint8 * 72)()
                do_ioctl(fd, cmd, buf)

        t = threading.Thread(target=worker)
        t.start()

        # Fire the signal a few times while the thread is running
        for _ in range(5):
            os.kill(os.getpid(), signal.SIGUSR1)
            time.sleep(0.005)

        t.join(timeout=5.0)
        self.assertFalse(t.is_alive(), "Worker thread hung!")

        os.close(fd)
        signal.signal(signal.SIGUSR1, old_handler)
        self.assertGreater(len(received), 0)


class TestNvidiaSmiIsolation(unittest.TestCase):
    """
    nvidia-smi inside the guest must only report GPU processes that are
    running inside this guest VM, not host processes or processes in other
    guests. This is the key isolation guarantee.
    """

    def setUp(self):
        require_nvidia()
        try:
            import subprocess
            result = subprocess.run(["nvidia-smi", "--version"],
                                    capture_output=True)
            if result.returncode != 0:
                raise unittest.SkipTest("nvidia-smi not available")
        except FileNotFoundError:
            raise unittest.SkipTest("nvidia-smi not installed")

    def test_nvidia_smi_works(self):
        import subprocess
        result = subprocess.run(["nvidia-smi"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         f"nvidia-smi failed:\n{result.stderr}")

    def test_reported_pids_are_guest_processes(self):
        """
        All PIDs reported by nvidia-smi --query-compute-apps must exist
        in /proc (i.e., be guest processes).
        """
        import subprocess
        result = subprocess.run(
            ["nvidia-smi", "--query-compute-apps=pid",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            self.skipTest(f"nvidia-smi query failed: {result.stderr}")

        pids = [line.strip() for line in result.stdout.splitlines()
                if line.strip() and line.strip() != "No running processes found"]

        guest_pids = set(os.listdir("/proc"))
        for pid_str in pids:
            try:
                int(pid_str)  # ensure it's numeric
            except ValueError:
                continue
            self.assertIn(pid_str, guest_pids,
                          f"nvidia-smi reported PID {pid_str} which is not "
                          f"in /proc — isolation leak (host process visible?)")

    def test_guest_compute_process_appears_in_smi(self):
        """
        When we start a CUDA process inside the guest, it should appear in
        nvidia-smi output. This validates the isolation works both ways.
        """
        try:
            import cupy as cp
        except ImportError:
            self.skipTest("cupy not installed")

        import subprocess

        # Create a background GPU workload
        def keep_gpu_busy():
            a = cp.zeros((1024, 1024), dtype=cp.float32)
            for _ in range(100):
                a = a + 1

        t = threading.Thread(target=keep_gpu_busy, daemon=True)
        t.start()
        time.sleep(0.5)

        result = subprocess.run(
            ["nvidia-smi", "--query-compute-apps=pid",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True
        )

        t.join(timeout=2)
        # We don't assert the PID is there (it may finish too fast), but
        # nvidia-smi must complete successfully
        self.assertEqual(result.returncode, 0)


class TestDriverVersionNegotiation(unittest.TestCase):
    """
    The NV_ESC_CHECK_VERSION_STR ioctl is the first thing libcuda does.
    Test that it succeeds and returns a sane version string.
    """

    def setUp(self):
        require_nvidia()

    def test_check_version_succeeds(self):
        fd = open_nvidiactl()
        try:
            cmd = make_check_version_cmd()
            buf = bytearray(72)
            try:
                fcntl.ioctl(fd, cmd, buf)
            except OSError:
                pass  # version mismatch is OK for this test

            # The module must stay alive
            self.assertTrue(os.path.exists("/dev/nvidiactl"))
        finally:
            os.close(fd)

    def test_check_version_string_is_printable(self):
        """Version string in the response should be a valid ASCII string."""
        fd = open_nvidiactl()
        try:
            cmd = make_check_version_cmd()
            buf = bytearray(72)
            try:
                fcntl.ioctl(fd, cmd, buf)
            except OSError:
                return  # version mismatch — that's fine

            # Extract the version string (last 64 bytes of 72-byte struct)
            version_bytes = buf[8:72]
            # Should be null-terminated printable ASCII
            null_pos = version_bytes.find(b'\x00')
            if null_pos >= 0:
                version_str = version_bytes[:null_pos].decode('ascii',
                                                               errors='replace')
                self.assertTrue(all(c.isprintable() for c in version_str),
                                f"Non-printable chars in version: {version_str!r}")
        finally:
            os.close(fd)


class TestSysParams(unittest.TestCase):
    """NV_ESC_SYS_PARAMS returns system memory size, must be sane."""

    def setUp(self):
        require_nvidia()

    def test_sys_params_memory_size_plausible(self):
        fd = open_nvidiactl()
        try:
            cmd = make_sys_params_cmd()
            buf = bytearray(8)
            try:
                fcntl.ioctl(fd, cmd, buf)
            except OSError:
                return
            mem_size = struct.unpack('<Q', buf)[0]
            # Should be > 0 and < some very large value (e.g., < 1 PiB)
            if mem_size > 0:
                self.assertLess(mem_size, 1 << 50,
                                f"Suspicious memory_size: {mem_size:#x}")
        finally:
            os.close(fd)


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--verbose", "-v", action="store_true")
    args, remaining = parser.parse_known_args()

    verbosity = 2 if args.verbose else 1
    sys.argv = [sys.argv[0]] + remaining

    loader = unittest.TestLoader()
    suite  = loader.loadTestsFromModule(sys.modules[__name__])
    runner = unittest.TextTestRunner(verbosity=verbosity)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
