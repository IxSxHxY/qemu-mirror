
/*
 * Host Memory Gateway (hostmem-gw) - map-on-demand RAM file window
 *
 * BAR0: control MMIO (registers)
 * BAR2: large container MemoryRegion (e.g., 4 GiB) into which we insert
 *       a RAM-from-file subregion on demand.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "include/qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "exec/memory.h"
#include "include/hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "qemu/bswap.h"
#include "qemu/units.h"

#include "qemu/uuid.h"
#include "hw/pci/pci_device.h"

#include "sysemu/hostmem.h"

#define TYPE_HOSTMEM_GW          "hostmem-gw"
#define HOSTMEM_GW(obj)          OBJECT_CHECK(HostMemGwState, (obj), TYPE_HOSTMEM_GW)

#define PCI_VENDOR_ID_PHISON   0x1987
#define PCI_DEVICE_ID_HOSTMEMGW 0x8299
/* ----- BAR0 register map ----- */
#define GW_REG_CMD               0x00  /* write: bit0=MAP, bit1=UNMAP */
#define   GW_CMD_MAP             (1u << 0)
#define   GW_CMD_UNMAP           (1u << 1)

#define GW_REG_STATUS            0x04  /* read: 0=OK, else error code */
#define GW_REG_PATH_LEN          0x08  /* write/read: bytes count (<= PATH_BUF_SIZE) */
#define GW_REG_SIZE_LO           0x0C  /* write/read: size low 32 bits */
#define GW_REG_SIZE_HI           0x10  /* write/read: size high 32 bits */
#define GW_REG_OFST_LO           0x14
#define GW_REG_OFST_HI           0x18

#define GW_REG_ERRNO             0x1C  /* read: internal errno for diagnostics */
#define GW_REG_PATH_BUF          0x20  /* write/read: path bytes start */

#define GW_PATH_BUF_SIZE         256
#define GW_BAR0_SIZE             0x200

/* Default whitelist prefix for safety; configurable via property */
#define GW_DEFAULT_PREFIX        "/mnt/hugepages/"

/* Status codes (simple) */
#define GW_STATUS_OK             0
#define GW_STATUS_EINVAL         22
#define GW_STATUS_ENOENT         2
#define GW_STATUS_E2BIG          7
#define GW_STATUS_EBUSY          16
#define GW_STATUS_EPERM          1

typedef struct HostMemGwState {
    PCIDevice      parent_obj;
    /* Regions */
    MemoryRegion   bar0_mmio;
    MemoryRegion   bar2_container;
    MemoryRegion   file_mr;            /* mapped RAM-from-file subregion */
    bool           file_mr_in_use;
    MemoryRegion   *bar2_hostmem_backend;

    /* Properties */
    HostMemoryBackend *hostmem;
    uint64_t       max_window_size;    /* BAR2 container size (default 4 GiB) */
    // bool           bar2_prefetchable;  /* PCI BAR prefetch hint */
    // char          *path_prefix;        /* whitelist path prefix */

    /* Control state (BAR0) */
    uint32_t       status;
    uint32_t       errno_val;
    uint32_t       path_len;
    uint64_t       req_size;
    uint64_t       ofst;
    char           path_buf[GW_PATH_BUF_SIZE + 1]; /* +1 for NUL */

} HostMemGwState;

/* ------------ Helpers ------------ */

static inline uint64_t clamp_window_size(uint64_t req, uint64_t maxwin)
{
    return (req > maxwin) ? maxwin : req;
}

static void gw_set_status(HostMemGwState *s, uint32_t st, uint32_t err)
{
    s->status = st;
    s->errno_val = err;
}


static void gw_unmap_file(HostMemGwState *s)
{
    // memory_region_transaction_begin();
    // if (s->file_mr_in_use) {
    //     memory_region_del_subregion(&s->bar2_container, &s->file_mr);
    //     //memory_region_destroy(&s->file_mr);
    //     s->file_mr_in_use = false;
    // }
    // memory_region_transaction_commit();
}

/* Map new file into BAR2 at offset 0 */
static void gw_map_file(HostMemGwState *s)
{
    // Error *local_err = NULL;

    // /* Sanity checks */
    // if (s->path_len == 0 || s->path_len > GW_PATH_BUF_SIZE) {
    //     gw_set_status(s, GW_STATUS_EINVAL, GW_STATUS_EINVAL);
    //     return;
    // }
    // s->path_buf[s->path_len] = '\0';

    // uint64_t size = clamp_window_size(s->req_size, s->max_window_size);
    // if (size == 0 || s->ofst >= size) {
    //     gw_set_status(s, GW_STATUS_EINVAL, GW_STATUS_EINVAL);
    //     return;
    // }

    // /* Perform mapping inside a memory transaction */
    // memory_region_transaction_begin();

    // /* Tear down previous subregion if present */
    // if (s->file_mr_in_use) {
    //     memory_region_del_subregion(&s->bar2_container, &s->file_mr);
    //     //memory_region_destroy(&s->file_mr);
    //     s->file_mr_in_use = false;
    // }

    // /* Map RAM from file; 'shared' means guest<->host visibility */
    
    // memory_region_init_ram_from_file(&s->file_mr, OBJECT(s),
    //                                  "hostmem-gw.file",
    //                                  size, /* bytes */
    //                                  4096,
    //                                  RAM_SHARED,
    //                                  s->path_buf,
    //                                  s->ofst,
    //                                  false,
    //                                  &local_err);

    // if (!local_err) {
    //     memory_region_add_subregion(&s->bar2_container, 0, &s->file_mr);
    //     s->file_mr_in_use = true;
    // }

    // memory_region_transaction_commit();

    // if (local_err) {
    //     /* Distinguish ENOENT vs generic EINVAL if possible */
    //     /* We don't have direct errno; classify as EINVAL by default. */
    //     gw_set_status(s, GW_STATUS_EINVAL, GW_STATUS_EINVAL);
    //     error_free(local_err);
    // } else {
    //     gw_set_status(s, GW_STATUS_OK, 0);
    // }
}

/* ------------ BAR0 MMIO ops ------------ */

static uint64_t hostmemgw_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    HostMemGwState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case GW_REG_STATUS:
        val = s->status;
        break;
    case GW_REG_PATH_LEN:
        val = s->path_len;
        break;
    case GW_REG_SIZE_LO:
        val = (uint32_t)(s->req_size & 0xffffffffu);
        break;
    case GW_REG_SIZE_HI:
        val = (uint32_t)((s->req_size >> 32) & 0xffffffffu);
        break;
    case GW_REG_OFST_LO:
        val = (uint32_t)(s->ofst & 0xffffffffu);
        break;
    case GW_REG_OFST_HI:
        val = (uint32_t)((s->ofst >> 32) & 0xffffffffu);
        break;
    case GW_REG_ERRNO:
        val = s->errno_val;
        break;
    default:
        if (addr >= GW_REG_PATH_BUF && addr < GW_REG_PATH_BUF + GW_PATH_BUF_SIZE) {
            /* Pack bytes from path buffer into the return value */
            hwaddr off = addr - GW_REG_PATH_BUF;
            unsigned i;
            for (i = 0; i < size && (off + i) < GW_PATH_BUF_SIZE; ++i) {
                val |= ((uint64_t)(uint8_t)s->path_buf[off + i]) << (8 * i);
            }
        } else {
            val = 0;
        }
        break;
    }

    return val;
}

static void hostmemgw_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    HostMemGwState *s = opaque;

    switch (addr) {
    case GW_REG_CMD: {
        /* Execute command */
        if (val & GW_CMD_MAP) {
            gw_map_file(s);
        }
        if (val & GW_CMD_UNMAP) {
            gw_unmap_file(s);
            gw_set_status(s, GW_STATUS_OK, 0);
        }
        break;
    }
    case GW_REG_PATH_LEN:
        s->path_len = (uint32_t)val;
        if (s->path_len > GW_PATH_BUF_SIZE) {
            s->path_len = GW_PATH_BUF_SIZE;
        }
        break;
    case GW_REG_SIZE_LO:
        s->req_size = (s->req_size & ~0xffffffffULL) | (uint32_t)val;
        break;
    case GW_REG_SIZE_HI:
        s->req_size = (s->req_size & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32);
        break;
    case GW_REG_OFST_LO:
        s->req_size = (s->req_size & ~0xffffffffULL) | (uint32_t)val;
        break;
    case GW_REG_OFST_HI:
        s->req_size = (s->req_size & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32);
        break;
    default:
        if (addr >= GW_REG_PATH_BUF && addr < GW_REG_PATH_BUF + GW_PATH_BUF_SIZE) {
            /* Unpack val into bytes and store in path buffer */
            hwaddr off = addr - GW_REG_PATH_BUF;
            unsigned i;
            for (i = 0; i < size && (off + i) < GW_PATH_BUF_SIZE; ++i) {
                s->path_buf[off + i] = (char)((val >> (8 * i)) & 0xffu);
            }
        }
        break;
    }
}

static const MemoryRegionOps hostmemgw_mmio_ops = {
    .read = hostmemgw_mmio_read,
    .write = hostmemgw_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 1,
    .impl.max_access_size  = 8,
};

/* ------------ QOM realize/reset/unrealize ------------ */

#include "include/hw/pci/pci.h"
#include "include/hw/pci/pcie.h"

// static bool hostmem_gw_init_pci(HostMemGwState *s, PCIDevice *pdev, Error **errp)
// {
//     ERRP_GUARD();
//     uint8_t *conf = pdev->config;

//     /* 1) Basic PCI header fields */
//     conf[PCI_INTERRUPT_PIN] = 1;                      // INTA (optional; you may skip INTx if only MSI-X)
//     pci_config_set_prog_interface(conf, 0x00);        // ProgIF: 0 for generic Memory device
//     pci_config_set_vendor_id(conf, 0x1234);           // your VID
//     pci_config_set_device_id(conf, 0x5678);           // your DID
//     pci_config_set_class(conf, PCI_CLASS_MEMORY_RAM); // matches the backing-memory nature

//     /* 2) Capabilities: make it PCIe + FLR (optional) */
//     // Power Management capability is optional for a simple memory window; skip unless you need it
//     // nvme uses 0x60; if you add PM, pick a non-conflicting offset.
//     pcie_endpoint_cap_init(pdev, 0x80);               // declare PCIe endpoint
//     pcie_cap_flr_init(pdev);                          // FLR helps with function-level resets

//     // /* 3) BAR setup: 64-bit MEM BAR2 holding your container */
//     // memory_region_init(&s->bar2_container, OBJECT(pdev),
//     //                    "hostmem-gw.bar2", s->max_window_size);

//     // /* no caching/prefetch for externally modified memory */
//     // uint8_t bar_flags = PCI_BASE_ADDRESS_SPACE_MEMORY |
//     //                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
//     //                     (s->bar2_prefetchable ? PCI_BASE_ADDRESS_MEM_PREFETCH : 0);

//     // /* Register BAR2 */
//     // pci_register_bar(pdev, 2, bar_flags, &s->bar2_container);

//     /* 4) (Optional) MSI-X if you later want notifications/interrupts
//      * If not, you can entirely skip MSI-X.
//      *
//      * Example layout: put MSI-X table & PBA at the end of BAR2
//      *
//      * size_t table_off = s->max_window_size - 0x2000; // ex., 8KB table
//      * size_t pba_off   = table_off + 0x1000;          // ex., 4KB PBA
//      * int ret = msix_init(pdev, s->msix_qsize,
//      *                     &s->bar2_container, 0, table_off,
//      *                     &s->bar2_container, 0, pba_off, 0, errp);
//      * if (ret == -ENOTSUP) { warn_report_err(*errp); *errp = NULL; }
//      * else if (ret < 0) { return false; }
//      */

//     /* 5) If you ever add SR-IOV, mirror NVMe’s PF/VF handling and ARI */
//     // pcie_ari_init(pdev, 0x100);          // only if you support ARI/SR-IOV
//     // pcie_sriov_vf_register_bar(...)      // for VF BARs

//     return true;
// }

static void hostmem_gw_realize(PCIDevice *pdev, Error **errp)
{
    HostMemGwState *s = HOSTMEM_GW(pdev);
    uint8_t *pci_conf;

    pci_conf = pdev->config;
    pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

    // hostmem_gw_init_pci(s, pdev, errp);
    /* Initialize BAR0 MMIO */
    memory_region_init_io(&s->bar0_mmio, OBJECT(pdev),
                          &hostmemgw_mmio_ops, s,
                          "hostmem-gw.ctrl",
                          GW_BAR0_SIZE);
    pci_register_bar(pdev, 0,
        PCI_BASE_ADDRESS_SPACE_MEMORY,
        &s->bar0_mmio);
    /* Initialize BAR2 as a large container region (64-bit) */
    memory_region_init(&s->bar2_container, OBJECT(pdev),
                       "hostmem-gw.bar2",
                       //s->max_window_size);
                       1024*1024*1024);

    Error *local_err = NULL;
    memory_region_transaction_begin();

    /* Map RAM from file; 'shared' means guest<->host visibility */
    
    memory_region_init_ram_from_file(&s->file_mr, OBJECT(s),
                                     "hostmem-gw.file",
                                     1024*1024, /* bytes */
                                     0,
                                     RAM_SHARED,
                                     "/dev/shm/ivshmem2",
                                     0,
                                    //  false,
                                     &local_err);
    if (!local_err) {
        memory_region_add_subregion(&s->bar2_container, 0, &s->file_mr);
        s->file_mr_in_use = true;
    }
    memory_region_transaction_commit();
    
    pci_register_bar(PCI_DEVICE(s), 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar2_container);
    
    // memory_region_init_io(&s->bar2_container, OBJECT(pdev),
    //                       &hostmemgw_mmio_ops, s,
    //                       "hostmem-gw.bar2",
    //                       GW_BAR0_SIZE);
    // s->req_size = 1024*1024U;
    // s->path_len = strlen("/var/tmp/gw-bar2.bin");
    // snprintf(s->path_buf, s->path_len + 1, "/var/tmp/gw-bar2.bin");
    // gw_map_file(s);
    
    // s->bar2_hostmem_backend = host_memory_backend_get_memory(s->hostmem);
    // host_memory_backend_set_mapped(s->hostmem, true);
    // //vmstate_register_ram(s->bar2_hostmem_backend, DEVICE(s));//only affects qemu migration, guest does not see this(i think)
    // pci_register_bar(pdev, 2,
    //                  PCI_BASE_ADDRESS_SPACE_MEMORY |
    //                  PCI_BASE_ADDRESS_MEM_PREFETCH |
    //                  PCI_BASE_ADDRESS_MEM_TYPE_64,
    //                  s->bar2_hostmem_backend);
    
    
    /* Initial state */
    // s->file_mr_in_use = false;
    // s->status         = GW_STATUS_OK;
    // s->errno_val      = 0;
    // s->path_len       = 0;
    // s->req_size       = s->max_window_size;  /* default full window */
    // memset(s->path_buf, 0, sizeof(s->path_buf));

    
}

static void hostmem_gw_unrealize(PCIDevice *pdev)
{
    HostMemGwState *s = HOSTMEM_GW(pdev);

    gw_unmap_file(s);
    // memory_region_destroy(&s->bar0_mmio);
    // memory_region_destroy(&s->bar2_container);
}

// static void hostmem_gw_reset(DeviceState *dev)
// {
//     HostMemGwState *s = HOSTMEM_GW(dev);
//     s->status    = GW_STATUS_OK;
//     s->errno_val = 0;
//     /* Keep previous mapping; reset only control plane */
// }

/* ------------ Properties & class ------------ */

static Property hostmem_gw_props[] = {
    /* Default BAR2 window size = 4 GiB (adjust as needed) */
    DEFINE_PROP_UINT64("max-window-size", HostMemGwState, max_window_size, 4*1024*1024*1024),
    DEFINE_PROP_LINK("memdev", HostMemGwState, hostmem, TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    /* Prefetch hint on the BAR2 PCI descriptor */
    // DEFINE_PROP_BOOL("prefetch", HostMemGwState, bar2_prefetchable, false),

    /* Whitelist prefix for paths written by guest (default: /mnt/hugepages/) */
    // DEFINE_PROP_STRING("path_prefix", HostMemGwState, path_prefix),

    DEFINE_PROP_END_OF_LIST(),
};

static void hostmem_gw_instance_init(Object *obj)
{
    // HostMemGwState *s = HOSTMEM_GW(obj);
    /* default prefix if none provided */
    // s->path_prefix = g_strdup(GW_DEFAULT_PREFIX);
}

static void hostmem_gw_class_init(ObjectClass *oc, void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize    = hostmem_gw_realize;
    pc->exit       = hostmem_gw_unrealize;
    pc->class_id   = PCI_CLASS_MEMORY_RAM;
    pc->revision   = 1;
    pc->vendor_id  = PCI_VENDOR_ID_PHISON;
    pc->device_id  = PCI_DEVICE_ID_HOSTMEMGW;
    
    // dc->reset      = hostmem_gw_reset;
    device_class_set_props(dc, hostmem_gw_props);
}

static const TypeInfo hostmem_gw_info = {
    .name          = TYPE_HOSTMEM_GW,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(HostMemGwState),
    .instance_init = hostmem_gw_instance_init,
    .class_init    = hostmem_gw_class_init,
    .interfaces = (InterfaceInfo[]) {
        //{ INTERFACE_PCIE_DEVICE },
        {INTERFACE_CONVENTIONAL_PCI_DEVICE},
        { }
    }
};

static void hostmem_gw_register_types(void)
{
    type_register_static(&hostmem_gw_info);
}

type_init(hostmem_gw_register_types);