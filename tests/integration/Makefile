CC     = gcc
CFLAGS = -Wall -Wextra -g -I../../src

all: test_ioctl_fwd fbo_formats_probe

test_ioctl_fwd: test_ioctl_fwd.c
	$(CC) $(CFLAGS) -o $@ $<

# Offscreen-EGL FBO completeness probe.  dlopen()s libEGL/libGLESv2, so -ldl is
# the only link dependency -- no EGL/GLES headers, no -dev packages.
fbo_formats_probe: fbo_formats_probe.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

run: test_ioctl_fwd
	./test_ioctl_fwd

clean:
	rm -f test_ioctl_fwd fbo_formats_probe
