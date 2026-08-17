KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# NVKVM_GRAPHICS=1 (default): full build — compute + DRM render node + virtual
#   KMS head (nvkvm_drm.o / nvkvm_kms.o); the "graphics" module version.
# NVKVM_GRAPHICS=0: compute-only build, like gVisor's nvproxy — no DRM, no KMS,
#   no /dev/nvidia-modeset surface. Drops nvkvm_drm.o / nvkvm_kms.o entirely so
#   the .ko carries zero graphics/display attack surface. Mirrors QEMU's
#   graphics=off gate (the two should be deployed consistently).
NVKVM_GRAPHICS ?= 1
export NVKVM_GRAPHICS

all:
	$(MAKE) -C $(KDIR) M=$(PWD) NVKVM_GRAPHICS=$(NVKVM_GRAPHICS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) NVKVM_GRAPHICS=$(NVKVM_GRAPHICS) modules_install
	depmod -a

.PHONY: all clean install
