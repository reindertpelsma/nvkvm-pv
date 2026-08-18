/*
 * virtio_nvgpu_pci.c — PCI transport wrapper for virtio-nvgpu
 *
 * Registers the "virtio-nvgpu-pci" device name so that QEMU guests can
 * instantiate the virtio-nvgpu device over PCI (the standard transport for
 * x86_64 KVM guests).
 *
 * This is a thin VirtIOPCIProxy wrapper — all device logic lives in
 * virtio_nvgpu.c.  The pattern mirrors hw/virtio/virtio-balloon-pci.c.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/qdev-properties.h"
#include "hw/pci/pci.h"
#include "exec/memory.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "virtio_nvgpu.h"

typedef struct VirtIONvgpuPCI VirtIONvgpuPCI;

#define TYPE_VIRTIO_NVGPU_PCI "virtio-nvgpu-pci-base"
DECLARE_INSTANCE_CHECKER(VirtIONvgpuPCI, VIRTIO_NVGPU_PCI,
                         TYPE_VIRTIO_NVGPU_PCI)

/*
 * #55 GPA-window reservation BAR.  We deliberately do NOT make this a
 * RAM-backed BAR: a RAM BAR gets a QEMU-listener-managed KVM memslot that
 * collides with the window's own raw KVM_SET_USER_MEMORY_REGION slot (proven by
 * the earlier probe — it broke cuInit).  Instead this is a pure MMIO BAR with
 * no backing: registering it makes the guest firmware ASSIGN and reserve a
 * 128 GiB 64-bit GPA range so QEMU/PCI never place another device or RAM there
 * (the only thing #55 needs).  The actual window RAM is still installed via the
 * raw KVM memslot at the BAR's firmware-assigned GPA — so we keep the raw path
 * (no QEMU dirty-map/madvise/fault-tracker) and that raw memslot shadows this
 * MMIO region, so these accessors are never invoked in practice.
 */
#define NVKVM_BAR_WINDOW       2
/* Size is NOT a constant any more: it follows the vdev's resolved sparse
 * window (nv->gpa.sparse_size), which realize may have shrunk to fit a narrow
 * host's physical address space.  A BAR larger than the window it reserves
 * would make firmware reserve — and possibly fail to place — a range we never
 * use.  NVKVM_SPARSE_GPA_SIZE remains the unshrunk default. */

struct VirtIONvgpuPCI {
	VirtIOPCIProxy parent_obj;
	VirtIONvgpu    vdev;
	MemoryRegion   window_bar;
};

static uint64_t nvkvm_winbar_read(void *opaque, hwaddr addr, unsigned size)
{
	(void)opaque; (void)addr; (void)size;
	return 0;  /* shadowed by the raw KVM memslot; never reached normally */
}

static void nvkvm_winbar_write(void *opaque, hwaddr addr, uint64_t val,
			       unsigned size)
{
	(void)opaque; (void)addr; (void)val; (void)size;
}

static const MemoryRegionOps nvkvm_winbar_ops = {
	.read       = nvkvm_winbar_read,
	.write      = nvkvm_winbar_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
};

/* #55: return the firmware-assigned GPA of the reservation BAR, or 0 if the
 * guest hasn't programmed it yet (PCI_BAR_UNMAPPED).  The vdev uses this as the
 * sparse window's base and raw-installs its KVM memslot there. */
static uint64_t nvkvm_pci_window_base(void *opaque)
{
	VirtIOPCIProxy *vpci_dev = opaque;
	uint64_t addr = vpci_dev->pci_dev.io_regions[NVKVM_BAR_WINDOW].addr;
	return (addr == PCI_BAR_UNMAPPED) ? 0 : addr;
}

static void virtio_nvgpu_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
	VirtIONvgpuPCI *dev  = VIRTIO_NVGPU_PCI(vpci_dev);
	DeviceState    *vdev = DEVICE(&dev->vdev);

	vpci_dev->class_code = PCI_CLASS_OTHERS;

	/*
	 * Enable MSI-X (one vector per VQ + 1 config) instead of falling back to
	 * legacy IO-APIC INTx.  Shared-INTx demux reads each sharing device's ISR
	 * status register over MMIO on EVERY completion interrupt — measured
	 * ~4170 IRQs/token + ~2150 ISR-read MMIO exits/token during LLM decode,
	 * the dominant decode tax (the block devices already use MSI-X; only this
	 * device was left on INTx because nvectors was unspecified).  3 VQs (tx,
	 * rx, evt) + 1 config vector.
	 */
	/* Force MSI-X: the struct default is 0 (not DEV_NVECTORS_UNSPECIFIED), so
	 * the usual "if unspecified" idiom never fires and we'd stay on INTx.  Set
	 * it unconditionally BEFORE qdev_realize — msix_init runs during the child
	 * device_plugged, so a later set is ignored. 3 VQs + 1 config. */
	if (vpci_dev->nvectors == DEV_NVECTORS_UNSPECIFIED || vpci_dev->nvectors == 0)
		vpci_dev->nvectors = 4;

	qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
	if (errp && *errp)
		return;

	/* #55: MMIO reservation BAR (no RAM, no memslot — avoids the probe's
	 * collision).  Firmware assigns its GPA; QEMU/PCI then reserve that
	 * range for us.  Size comes from the vdev's realize-time GPA layout
	 * (qdev_realize above has already run), so on a narrow host the BAR
	 * shrinks together with the window instead of asking firmware for a
	 * 128 GiB range that cannot be placed. */
	uint64_t bar_size = dev->vdev.gpa.sparse_size;
	if (bar_size == 0)
		bar_size = NVKVM_SPARSE_GPA_SIZE;
	memory_region_init_io(&dev->window_bar, OBJECT(dev), &nvkvm_winbar_ops,
			      dev, "nvkvm-gpa-window", bar_size);
	pci_register_bar(&vpci_dev->pci_dev, NVKVM_BAR_WINDOW,
			 PCI_BASE_ADDRESS_SPACE_MEMORY |
			 PCI_BASE_ADDRESS_MEM_TYPE_64 |
			 PCI_BASE_ADDRESS_MEM_PREFETCH,
			 &dev->window_bar);

	/* Let the vdev resolve the window base from this BAR (lazily, once the
	 * guest has programmed it). */
	dev->vdev.window_base_get     = nvkvm_pci_window_base;
	dev->vdev.window_base_opaque  = vpci_dev;
}

static void virtio_nvgpu_pci_class_init(ObjectClass *klass, void *data)
{
	DeviceClass     *dc       = DEVICE_CLASS(klass);
	VirtioPCIClass  *k        = VIRTIO_PCI_CLASS(klass);
	PCIDeviceClass  *pcidev_k = PCI_DEVICE_CLASS(klass);

	k->realize = virtio_nvgpu_pci_realize;
	set_bit(DEVICE_CATEGORY_MISC, dc->categories);

	/* virtio modern PCI device IDs: vendor 0x1AF4, device 0x1040 + type_id */
	pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
	pcidev_k->device_id = 0x1040 + VIRTIO_ID_NVGPU; /* 0x1072 */
	pcidev_k->revision  = VIRTIO_PCI_ABI_VERSION;
	pcidev_k->class_id  = PCI_CLASS_OTHERS;
}

static void virtio_nvgpu_pci_instance_init(Object *obj)
{
	VirtIONvgpuPCI *dev = VIRTIO_NVGPU_PCI(obj);

	virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
				    TYPE_VIRTIO_NVGPU);
}

static const VirtioPCIDeviceTypeInfo virtio_nvgpu_pci_info = {
	.base_name             = TYPE_VIRTIO_NVGPU_PCI,
	.generic_name          = "virtio-nvgpu-pci",
	.transitional_name     = "virtio-nvgpu-pci-transitional",
	.non_transitional_name = "virtio-nvgpu-pci-non-transitional",
	.instance_size         = sizeof(VirtIONvgpuPCI),
	.instance_init         = virtio_nvgpu_pci_instance_init,
	.class_init            = virtio_nvgpu_pci_class_init,
};

static void virtio_nvgpu_pci_register(void)
{
	virtio_pci_types_register(&virtio_nvgpu_pci_info);
}
type_init(virtio_nvgpu_pci_register)
