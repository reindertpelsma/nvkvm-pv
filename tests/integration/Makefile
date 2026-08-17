CC     = gcc
CFLAGS = -Wall -Wextra -g -I../../src

all: test_ioctl_fwd

test_ioctl_fwd: test_ioctl_fwd.c
	$(CC) $(CFLAGS) -o $@ $<

run: test_ioctl_fwd
	./test_ioctl_fwd

clean:
	rm -f test_ioctl_fwd
