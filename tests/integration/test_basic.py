#!/usr/bin/env python3
"""
test_basic.py — Basic CUDA smoke tests for the nvkvm guest module.

Run inside a guest VM that has nvkvm-guest.ko loaded and /dev/nvidia*
exposed by the virtio-nvgpu backend.

Requirements:
    pip install cupy-cuda12x   # or matching CUDA version

Usage:
    python3 test_basic.py [--verbose]
"""

import sys
import os
import argparse
import unittest

def require_nvidia():
    if not os.path.exists("/dev/nvidiactl"):
        raise RuntimeError("/dev/nvidiactl not found — is nvkvm-guest.ko loaded?")


class TestDeviceFiles(unittest.TestCase):
    """Verify that /dev/nvidia* device nodes are present and openable."""

    def test_nvidiactl_exists(self):
        require_nvidia()
        self.assertTrue(os.path.exists("/dev/nvidiactl"),
                        "/dev/nvidiactl missing")

    def test_nvidia_uvm_exists(self):
        require_nvidia()
        self.assertTrue(os.path.exists("/dev/nvidia-uvm"),
                        "/dev/nvidia-uvm missing")

    def test_nvidia0_exists(self):
        require_nvidia()
        self.assertTrue(os.path.exists("/dev/nvidia0"),
                        "/dev/nvidia0 missing")

    def test_open_nvidiactl(self):
        require_nvidia()
        fd = os.open("/dev/nvidiactl", os.O_RDWR)
        self.assertGreater(fd, 0)
        os.close(fd)

    def test_open_nvidia0(self):
        require_nvidia()
        fd = os.open("/dev/nvidia0", os.O_RDWR)
        self.assertGreater(fd, 0)
        os.close(fd)


class TestNvidiaVersion(unittest.TestCase):
    """Check that nvidia-smi and nvcc report sensible versions."""

    def test_nvidia_smi(self):
        require_nvidia()
        ret = os.system("nvidia-smi -L > /dev/null 2>&1")
        self.assertEqual(ret, 0, "nvidia-smi failed")

    def test_nvidia_smi_reports_gpu(self):
        require_nvidia()
        import subprocess
        out = subprocess.check_output(["nvidia-smi", "-L"],
                                       stderr=subprocess.DEVNULL).decode()
        self.assertIn("GPU", out, "No GPU reported by nvidia-smi")


class TestCUDABasic(unittest.TestCase):
    """Basic CUDA allocation and kernel execution via CuPy."""

    @classmethod
    def setUpClass(cls):
        require_nvidia()
        try:
            import cupy as cp
            cls.cp = cp
        except ImportError:
            raise unittest.SkipTest("cupy not installed — skipping CUDA tests")

    def test_device_count(self):
        cp = self.cp
        count = cp.cuda.runtime.getDeviceCount()
        self.assertGreater(count, 0, "No CUDA devices found")

    def test_array_allocation(self):
        cp = self.cp
        a = cp.zeros((1024,), dtype=cp.float32)
        self.assertEqual(a.shape, (1024,))

    def test_array_fill_and_transfer(self):
        cp = self.cp
        import numpy as np
        a = cp.full((256,), 3.14, dtype=cp.float32)
        b = cp.asnumpy(a)
        self.assertTrue(np.allclose(b, 3.14),
                        "GPU→CPU transfer mismatch")

    def test_element_wise_add(self):
        cp = self.cp
        import numpy as np
        x = cp.arange(1000, dtype=cp.float32)
        y = x + x
        result = cp.asnumpy(y)
        expected = np.arange(1000, dtype=np.float32) * 2
        self.assertTrue(np.allclose(result, expected))

    def test_matmul(self):
        cp = self.cp
        import numpy as np
        A = cp.random.rand(64, 64).astype(cp.float32)
        B = cp.random.rand(64, 64).astype(cp.float32)
        C = A @ B
        self.assertEqual(C.shape, (64, 64))
        # Verify against CPU
        A_np = cp.asnumpy(A)
        B_np = cp.asnumpy(B)
        C_np = cp.asnumpy(C)
        self.assertTrue(np.allclose(C_np, A_np @ B_np, atol=1e-3))

    def test_pinned_memory(self):
        """cudaMallocHost — tests the OS descriptor / mmap path."""
        cp = self.cp
        mem = cp.cuda.alloc_pinned_memory(4096)
        self.assertIsNotNone(mem)
        del mem

    def test_device_synchronize(self):
        cp = self.cp
        cp.cuda.Device().synchronize()


class TestPyTorchBasic(unittest.TestCase):
    """Smoke test PyTorch GPU operations."""

    @classmethod
    def setUpClass(cls):
        require_nvidia()
        try:
            import torch
            if not torch.cuda.is_available():
                raise unittest.SkipTest("torch.cuda not available")
            cls.torch = torch
        except ImportError:
            raise unittest.SkipTest("torch not installed")

    def test_cuda_available(self):
        torch = self.torch
        self.assertTrue(torch.cuda.is_available())

    def test_tensor_to_gpu(self):
        torch = self.torch
        t = torch.zeros(128, 128).cuda()
        self.assertEqual(t.device.type, "cuda")

    def test_tensor_add(self):
        torch = self.torch
        import numpy as np
        a = torch.ones(256, device="cuda")
        b = torch.ones(256, device="cuda")
        c = (a + b).cpu().numpy()
        self.assertTrue(np.allclose(c, 2.0))

    def test_linear_forward(self):
        torch = self.torch
        model = torch.nn.Linear(128, 64).cuda()
        x = torch.randn(8, 128, device="cuda")
        out = model(x)
        self.assertEqual(out.shape, (8, 64))

    def test_backward_pass(self):
        torch = self.torch
        model = torch.nn.Linear(32, 16).cuda()
        x = torch.randn(4, 32, device="cuda", requires_grad=True)
        loss = model(x).sum()
        loss.backward()
        for p in model.parameters():
            self.assertIsNotNone(p.grad)


class TestIsolation(unittest.TestCase):
    """
    Verify that the guest cannot see CUDA contexts from other processes
    (analogous to gVisor's container isolation for nvidia-smi).
    """

    @classmethod
    def setUpClass(cls):
        require_nvidia()
        try:
            import pynvml
            pynvml.nvmlInit()
            cls.pynvml = pynvml
        except (ImportError, Exception):
            raise unittest.SkipTest("pynvml not available")

    def test_gpu_visible(self):
        nvml = self.pynvml
        count = nvml.nvmlDeviceGetCount()
        self.assertGreater(count, 0)

    def test_only_guest_processes_visible(self):
        """
        nvidia-smi / NVML should only report compute processes running in
        this guest, not processes from the host or other guests.
        This is enforced by the guest kernel module's session isolation.
        """
        nvml  = self.pynvml
        handle = nvml.nvmlDeviceGetHandleByIndex(0)
        procs = nvml.nvmlDeviceGetComputeRunningProcesses(handle)
        guest_pids = set(os.listdir("/proc"))
        for proc in procs:
            self.assertIn(str(proc.pid), guest_pids,
                          f"PID {proc.pid} visible in nvidia-smi but not "
                          f"in /proc — possible isolation leak")


if __name__ == "__main__":
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
