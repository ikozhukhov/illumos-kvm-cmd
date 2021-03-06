/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2012 Joyent, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/mdb_modapi.h>

#include "hw.h"
#include "pci.h"
#include "hw/virtio.h"
#include "hw/virtio-net.h"
#include "pci_internals.h"
#include "qemu-queue.h"

/*
 * Sigh, this isn't of course defined in any header file, so we just have to
 * #include this ourselves.
 */
struct PCIHostBus {
	int domain;
	struct PCIBus *bus;
	QLIST_ENTRY(PCIHostBus) next;
};

typedef struct {
	PCIDevice pci_dev;
	VirtIODevice *vdev;
	uint32_t flags;
	uint32_t addr;
	uint32_t class_code;
	uint32_t nvectors;
	BlockConf block;
	NICConf nic;
	uint32_t host_features;
#ifdef CONFIG_LINUX
	V9fsConf fsconf;
#endif
	/* Max. number of ports we can have for a the virtio-serial device */
	uint32_t max_virtserial_ports;
	virtio_net_conf net;
	bool ioeventfd_disabled;
	bool ioeventfd_started;
} VirtIOPCIProxy;

typedef struct RAMBlock {
	uint8_t *host;
	ram_addr_t offset;
	ram_addr_t length;
	char idstr[256];
	QLIST_ENTRY(RAMBlock) next;
#if defined(__linux__) && !defined(TARGET_S390X)
	int fd;
#endif
} RAMBlock;

typedef struct RAMList {
	uint8_t *phys_dirty;
	QLIST_HEAD(ram, RAMBlock) blocks;
} RAMList;

typedef struct VRingDesc
{
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} VRingDesc;

typedef struct VRingAvail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[0];
} VRingAvail;

typedef struct VRingUsedElem
{
    uint32_t id;
    uint32_t len;
} VRingUsedElem;

typedef struct VRingUsed
{
    uint16_t flags;
    uint16_t idx;
    VRingUsedElem ring[0];
} VRingUsed;

typedef struct VRing
{
    unsigned int num;
    target_phys_addr_t desc;
    target_phys_addr_t avail;
    target_phys_addr_t used;
} VRing;

/*
 * NDEVICES comes from the PCIDevice structure and should be changed if this
 * does ever change.
 */
#define	NDEVICES	256
typedef struct pci_dev_wdata {
	struct PCIDevice	*pdw_devs[NDEVICES];
	int			pdw_idx;
} pci_dev_wdata_t;

static int
qemu_mdb_host_bus_init(mdb_walk_state_t *wsp)
{
	struct PCIHostBus *head;
	GElf_Sym sym;

	if (wsp->walk_addr != NULL) {
		mdb_printf("qemu_host_bus does not support local walks");
		return (WALK_ERR);
	}

	/*
	 * The root of the host busses is defined in QEMU as:
	 * static QLIST_HEAD(, PCIHostBus) host_buses;
	 *
	 * However we don't really get a type, it's basically an anoynmous
	 * struct to mdb. So instead, since the head of the queue list points to
	 * the first one, we tread it as a struct PCIHOSTBus**.
	 */
	if (mdb_lookup_by_name("host_buses", &sym) != 0) {
		mdb_warn("unable to locate host_buse");
		return (WALK_ERR);
	}

	if (mdb_vread(&head, sizeof (head), sym.st_value) != sizeof (head)) {
		mdb_warn("failed to read host_buses");
		return (WALK_ERR);
	}

	wsp->walk_addr = (uintptr_t)head;
	if (head == NULL)
		return (WALK_DONE);

	return (WALK_NEXT);
}


static int
qemu_mdb_host_bus_step(mdb_walk_state_t *wsp)
{
	struct PCIHostBus bus;
	uintptr_t addr = wsp->walk_addr;

	if (addr == NULL)
		return (WALK_DONE);

	if (mdb_vread(&bus, sizeof (bus), addr) != sizeof (bus)) {
		mdb_warn("failed to read struct PCIHostBus");
		return (WALK_ERR);
	}

	wsp->walk_addr = (uintptr_t)bus.next.le_next;
	return (wsp->walk_callback(addr, &bus, wsp->walk_cbdata));
}

static int
qemu_mdb_pci_device_init(mdb_walk_state_t *wsp)
{
	int ii;
	struct PCIBus bus;
	struct PCIHostBus host;
	pci_dev_wdata_t *pdw;
	struct PCIHostBus *headp;
	GElf_Sym sym;
	uintptr_t baddr;

	/*
	 * We're going to make some great assumptions here, which in practice
	 * have been proven true in so far as we care about. Basically that
	 * there is only one HostBus and that that HostBus in reality only has
	 * one PCIBus which is the one we care about. So that's what we do here.
	 */
	if (wsp->walk_addr == NULL) {
		if (mdb_lookup_by_name("host_buses", &sym) != 0) {
			mdb_warn("unable to locate host_buse");
			return (WALK_ERR);
		}

		if (mdb_vread(&headp, sizeof (headp), sym.st_value) !=
		    sizeof (headp)) {
			mdb_warn("failed to read host_buses");
			return (WALK_ERR);
		}

		if (mdb_vread(&host, sizeof (host), (uintptr_t)headp) !=
		    sizeof (host)) {
			mdb_warn("failed to read host bus");
			return (WALK_ERR);
		}

		baddr = (uintptr_t)host.bus;
	} else {
		baddr = wsp->walk_addr;
	}

	if (mdb_vread(&bus, sizeof (bus), baddr) != sizeof (bus)) {
		mdb_warn("failed to read PCIBus\n");
		return (WALK_ERR);
	}

	pdw = mdb_zalloc(sizeof (pci_dev_wdata_t), UM_SLEEP | UM_GC);
	(void) bcopy(bus.devices, pdw->pdw_devs, sizeof (bus.devices));

	/*
	 * Find the first device.
	 */
	for (ii = 0; ii < NDEVICES; ii++)
		if (pdw->pdw_devs[ii] != NULL)
			break;

	if (ii == NDEVICES)
		return (WALK_DONE);

	pdw->pdw_idx = ii;
	wsp->walk_addr = (uintptr_t)pdw->pdw_devs[ii];
	wsp->walk_data = pdw;

	return (WALK_NEXT);
}

static int
qemu_mdb_pci_device_step(mdb_walk_state_t *wsp)
{
	PCIDevice dev;
	pci_dev_wdata_t *pdw = wsp->walk_data;
	uintptr_t addr = wsp->walk_addr;
	int ii;

	if (pdw->pdw_idx == NDEVICES)
		return (WALK_DONE);

	if (mdb_vread(&dev, sizeof (dev), addr) != sizeof (dev)) {
		mdb_warn("couldn't read PCIDevice at %p", addr);
		return (WALK_ERR);
	}

	for (ii = pdw->pdw_idx + 1; ii < NDEVICES; ii++)
		if (pdw->pdw_devs[ii] != NULL)
			break;

	pdw->pdw_idx = ii;
	if (ii == NDEVICES)
		wsp->walk_addr = NULL;
	else
		wsp->walk_addr = (uintptr_t)pdw->pdw_devs[ii];

	return (wsp->walk_callback(addr, &dev, wsp->walk_cbdata));
}


static int
qemu_mdb_pci_dev_type_init(mdb_walk_state_t *wsp)
{
	if (wsp->walk_addr != NULL) {
		mdb_warn("local walks not supported");
		return (WALK_ERR);
	}

	if (wsp->walk_arg == NULL) {
		mdb_warn("called into qemu_mdb_pci_dev_type_init with no arg");
		return (WALK_ERR);
	}

	if (mdb_layered_walk("qemu_pci_device", wsp) == -1) {
		mdb_warn("failed to init layered walk");
		return (WALK_ERR);
	}

	return (WALK_NEXT);
}

static int
qemu_mdb_pci_dev_type_step(mdb_walk_state_t *wsp)
{
	PCIDevice dev;

	if (wsp->walk_addr == NULL) {
		mdb_warn("found unexpected null device pointer");
		return (WALK_ERR);
	}

	if (mdb_vread(&dev, sizeof (dev), wsp->walk_addr) != sizeof (dev)) {
		mdb_warn("failed to read device: %p", wsp->walk_addr);
		return (WALK_ERR);
	}

	if (strcmp(wsp->walk_arg, dev.name) != 0)
		return (WALK_NEXT);

	return (wsp->walk_callback(wsp->walk_addr, &dev, wsp->walk_cbdata));
}


/*
 * XXX There is a subtle mdb memory leak here. We're duping the string name for
 * the walkers as initial arguments so we can use it as a filter when doing the
 * larger walk. This is fine, but right now we're being rather lazy and not
 * cleaning up that these exist which means that we need some way to keep track
 * of them at some point and free it when we unload.
 */
/*ARGSUSED*/
static int
qemu_mdb_init_walkers(uintptr_t addr, const PCIDevice *d, void *ignored)
{
	mdb_walker_t w;
	size_t len;
	char *ndup;
	char wname[64];
	char descr[64];

	(void) mdb_snprintf(descr, sizeof (descr),
	    "walk the qemu %s devices", d->name);
	(void) mdb_snprintf(wname, sizeof (wname),
	    "qemu_%s", d->name);

	/* Don't forget your null terminator */
	len = strlen(d->name) + 1;
	ndup = mdb_alloc(sizeof (char) * len, UM_SLEEP);
	(void) strcpy(ndup, d->name);
	w.walk_name = wname;
	w.walk_descr = descr;
	w.walk_init = qemu_mdb_pci_dev_type_init;
	w.walk_step = qemu_mdb_pci_dev_type_step;
	w.walk_fini = NULL;
	w.walk_init_arg = (void *)ndup;

	/*
	 * XXX Normally this failure would be bad, but we're purposefully being
	 * lazy and recreating walkers with the same name as ones which already
	 * exist, e.g. when we have multiple devices of the same general type.
	 *
	 * Unfortunately, errno's aren't part of the module API so we have no
	 * way of distinguishing them. So we just swallow all of them for now.
	 */
	if (mdb_add_walker(&w) == -1)
		mdb_free(ndup, len * sizeof (char));

	return (0);
}

static int
qemu_mdb_init(void)
{

	mdb_walker_t w = { "qemu_pci_device",
		"walk a PCI Bus's attached devices", qemu_mdb_pci_device_init,
		qemu_mdb_pci_device_step, NULL };

	if (mdb_add_walker(&w) == -1) {
		mdb_warn("failed to add qemu_pci_device walker");
		return (-1);
	}

	(void) mdb_walk("qemu_pci_device", (mdb_walk_cb_t)qemu_mdb_init_walkers,
	    NULL);
	return (0);
}

static int
qemu_mdb_pcidev2virtio(uintptr_t addr, uint_t flags, int argc,
    const mdb_arg_t *argv)
{
	VirtIOPCIProxy v;

	if (!(flags & DCMD_ADDRSPEC))
		return (DCMD_USAGE);

	if (argc > 1)
		return (DCMD_USAGE);

	if (mdb_vread(&v, sizeof (v), addr) != sizeof (v)) {
		mdb_warn("failed to read Virtio Proxy structure");
		return (DCMD_ERR);
	}

	mdb_printf("%lr\n", v.vdev);

	return (DCMD_OK);
}

/*
 * These are a series of definitions that we need for qemu_mdb_tpa2qva. Note
 * that while most of them have the same name, unofrutnately qemu has a #pragma
 * poinson on some of them that prevents us from using them without changing the
 * name.
 */
typedef struct PhysPageDesc {
	/* offset in host memory of the page + io_index in the low bits */
	ram_addr_t phys_offset;
	ram_addr_t region_offset;
} PhysPageDesc;

#define	MDB_TARGET_PAGE_BITS 12
#define	MDB_TARGET_PAGE_SIZE (1 << MDB_TARGET_PAGE_BITS)
#define	MDB_TARGET_PAGE_MASK ~(MDB_TARGET_PAGE_SIZE - 1)
#define	TARGET_VIRT_ADDR_SPACE_BITS 47
#define	TARGET_PHYS_ADDR_SPACE_BITS 52
#define	L2_BITS 10
#define	L2_SIZE (1 << L2_BITS)
#define	P_L1_BITS_REM \
	((TARGET_PHYS_ADDR_SPACE_BITS - MDB_TARGET_PAGE_BITS) % L2_BITS)
#if P_L1_BITS_REM < 4
#define	P_L1_BITS  (P_L1_BITS_REM + L2_BITS)
#else
#define	P_L1_BITS  P_L1_BITS_REM
#endif
#define	P_L1_SIZE  ((uintptr_t)1 << P_L1_BITS)
#define	P_L1_SHIFT (TARGET_PHYS_ADDR_SPACE_BITS - MDB_TARGET_PAGE_BITS - \
    P_L1_BITS)

static uintptr_t
qemu_mdb_get_ram_ptr(uintptr_t addr)
{
	GElf_Sym sym;
	RAMList rl;
	uintptr_t rbp;
	RAMBlock rb;

	if (mdb_lookup_by_name("ram_list", &sym) != 0) {
		mdb_warn("failed to look up ram_list");
		return (0);
	}

	if (mdb_vread(&rl, sizeof (rl), sym.st_value) != sizeof (rl)) {
		mdb_warn("failed to read ram_list");
		return (0);
	}

	rbp = (uintptr_t)rl.blocks.lh_first;
	for (;;) {
		if (rbp == NULL) {
			mdb_warn("failed to find RAMBlock for address");
			return (0);
		}

		if (mdb_vread(&rb, sizeof (rb), rbp) != sizeof (rb)) {
			mdb_warn("failed to read RAMBlock %p", rbp);
			return (0);
		}

		if (addr - rb.offset < rb.length)
			break;

		rbp = (uintptr_t)rb.next.le_next;
	}

	return ((uintptr_t)(rb.host + (addr - rb.offset)));
}

static int
internal_tpa2qva(uintptr_t addr, uintptr_t *res)
{
	GElf_Sym sym;
	void **lp, **p;
	int ii;
	PhysPageDesc *pdp, pd;
	uintptr_t paddr, pfaddr, vptr;

	if (mdb_lookup_by_name("l1_phys_map", &sym) != 0) {
		mdb_warn("unable to locate host_buse");
		return (DCMD_ERR);
	}

	lp = (void **)sym.st_value;
	pfaddr = addr >> MDB_TARGET_PAGE_BITS;
	lp += ((pfaddr >> P_L1_SHIFT) & (P_L1_SIZE - 1));

	for (ii = P_L1_SHIFT / L2_BITS - 1; ii > 0; ii--) {
		if (mdb_vread(&p, sizeof (p), (uintptr_t)lp) != sizeof (p)) {
			mdb_warn("failed to read into l1 page table");
			return (DCMD_ERR);
		}

		if (p == NULL) {
			mdb_warn("found a null entry, bailing");
			return (DCMD_ERR);
		}

		lp = p + ((pfaddr >> (ii * L2_BITS)) & (L2_SIZE - 1));
	}

	if (mdb_vread(&pdp, sizeof (pdp), (uintptr_t)lp) != sizeof (pdp)) {
		mdb_warn("failed to read into the PhysPageDesc");
		return (DCMD_ERR);
	}

	if (pdp == NULL) {
		mdb_warn("found null PhysPageDesc, bailing");
		return (DCMD_ERR);
	}

	pdp += (pfaddr & (L2_SIZE - 1));
	if (mdb_vread(&pd, sizeof (pd), (uintptr_t)pdp) != sizeof (pd)) {
		mdb_warn("failed to read pdp");
		return (DCMD_ERR);
	}

	paddr = pd.phys_offset;
	if ((paddr & ~MDB_TARGET_PAGE_MASK) > IO_MEM_ROM &&
	    !(paddr & IO_MEM_ROMD)) {
		mdb_printf("Address is in I/O space. Not touching it.");
		return (DCMD_OK);
	}

	vptr = qemu_mdb_get_ram_ptr(paddr & MDB_TARGET_PAGE_MASK);
	if (vptr == 0)
		return (DCMD_ERR);
	vptr += addr & ~MDB_TARGET_PAGE_MASK;
	*res = vptr;

	return (DCMD_OK);
}

static int
qemu_mdb_tpa2qva(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uintptr_t vptr;

	if (!(flags & DCMD_ADDRSPEC))
		return (DCMD_USAGE);

	if (argc > 1)
		return (DCMD_USAGE);

	if (internal_tpa2qva(addr, &vptr) != DCMD_OK)
		return (DCMD_ERR);

	mdb_printf("%lr\n", vptr);

	return (DCMD_OK);
}

static int
qemu_mdb_vrused(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	VRing ring;
	uintptr_t avaddr;
	uint16_t index;

	if (!(flags & DCMD_ADDRSPEC))
		return (DCMD_USAGE);

	if (argc > 1)
		return (DCMD_USAGE);

	if (mdb_vread(&ring, sizeof (ring), addr) != sizeof (ring)) {
		mdb_warn("failed to read VRing");
		return (DCMD_ERR);
	}

	if (internal_tpa2qva(ring.avail, &avaddr) != DCMD_OK) {
		mdb_warn("failed to translate available ring to VA");
		return (DCMD_ERR);
	}

	/* Account for offset */
	avaddr += ring.num * sizeof (uint16_t) + 0x4;
	if (mdb_vread(&index, sizeof (index), avaddr) != sizeof (index)) {
		mdb_warn("failed to read index value");
		return (DCMD_ERR);
	}

	mdb_printf("%lr\n", index);
	return (DCMD_OK);
}

static int
qemu_mdb_vravail(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	VRing ring;
	uintptr_t avaddr;
	uint16_t index;

	if (!(flags & DCMD_ADDRSPEC))
		return (DCMD_USAGE);

	if (argc > 1)
		return (DCMD_USAGE);

	if (mdb_vread(&ring, sizeof (ring), addr) != sizeof (ring)) {
		mdb_warn("failed to read VRing");
		return (DCMD_ERR);
	}

	if (internal_tpa2qva(ring.used, &avaddr) != DCMD_OK) {
		mdb_warn("failed to translate available ring to VA");
		return (DCMD_ERR);
	}

	/* Account for offset */
	avaddr += ring.num * sizeof (uint64_t) + 0x4;
	if (mdb_vread(&index, sizeof (index), avaddr) != sizeof (index)) {
		mdb_warn("failed to read index value");
		return (DCMD_ERR);
	}

	mdb_printf("%lr\n", index);
	return (DCMD_OK);
}

static const mdb_dcmd_t qemu_dcmds[] = {
	{ "pcidev2virtio", NULL, "translate a virtio PCI device to its "
		"virtio equivalent", qemu_mdb_pcidev2virtio },
	{ "qemu_tpa2qva", NULL, "translate a target physical address to a "
		"QEMU virtual address", qemu_mdb_tpa2qva },
	{ "qemu_vrused", NULL, "Spit out the used event of the vring",
		qemu_mdb_vrused },
	{ "qemu_vravail", NULL, "Spit out the avail event of the vring",
		qemu_mdb_vravail },
	{ NULL }
};

static const mdb_walker_t qemu_walkers[] = {
	{ "qemu_host_bus", "walk qemu PCIHostBus structures",
		qemu_mdb_host_bus_init, qemu_mdb_host_bus_step, NULL },
	{ NULL }
};

static const mdb_modinfo_t qemu_mdb_modinfo = { MDB_API_VERSION, qemu_dcmds,
	qemu_walkers };

const mdb_modinfo_t *
_mdb_init(void)
{
	if (qemu_mdb_init() != 0)
		return (NULL);

	return (&qemu_mdb_modinfo);
}
