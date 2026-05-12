/*
 * QEMU Educational GPGPU Device
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#include "gpgpu.h"
#include "gpgpu_core.h"

static void gpgpu_update_intx(GPGPUState *s);
static void gpgpu_raise_irq(GPGPUState *s, uint32_t irq, unsigned vector);
static void gpgpu_dma_complete(void *opaque);
static void gpgpu_kernel_complete(void *opaque);

/* TODO: Implement MMIO control register read */
static uint64_t gpgpu_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)size;

    GPGPUState *s = opaque;

    switch (addr) {
    case GPGPU_REG_DEV_ID:
        return GPGPU_DEV_ID_VALUE;
    case GPGPU_REG_DEV_VERSION:
        return GPGPU_DEV_VERSION_VALUE;
    case GPGPU_REG_DEV_CAPS:
        return (s->warp_size & 0xff) |
               ((s->warps_per_cu & 0xff) << 8) |
               ((s->num_cus & 0xff) << 16);
    case GPGPU_REG_VRAM_SIZE_LO:
        return (uint32_t)s->vram_size;
    case GPGPU_REG_VRAM_SIZE_HI:
        return (uint32_t)(s->vram_size >> 32);
    case GPGPU_REG_GLOBAL_CTRL:
        return s->global_ctrl;
    case GPGPU_REG_GLOBAL_STATUS:
        return s->global_status;
    case GPGPU_REG_ERROR_STATUS:
        return s->error_status;
    case GPGPU_REG_IRQ_ENABLE:
        return s->irq_enable;
    case GPGPU_REG_IRQ_STATUS:
        return s->irq_status;
    case GPGPU_REG_IRQ_ACK:
        return 0;
    case GPGPU_REG_KERNEL_ADDR_LO:
        return (uint32_t)s->kernel.kernel_addr;
    case GPGPU_REG_KERNEL_ADDR_HI:
        return (uint32_t)(s->kernel.kernel_addr >> 32);
    case GPGPU_REG_KERNEL_ARGS_LO:
        return (uint32_t)s->kernel.kernel_args;
    case GPGPU_REG_KERNEL_ARGS_HI:
        return (uint32_t)(s->kernel.kernel_args >> 32);
    case GPGPU_REG_GRID_DIM_X:
        return s->kernel.grid_dim[0];
    case GPGPU_REG_GRID_DIM_Y:
        return s->kernel.grid_dim[1];
    case GPGPU_REG_GRID_DIM_Z:
        return s->kernel.grid_dim[2];
    case GPGPU_REG_BLOCK_DIM_X:
        return s->kernel.block_dim[0];
    case GPGPU_REG_BLOCK_DIM_Y:
        return s->kernel.block_dim[1];
    case GPGPU_REG_BLOCK_DIM_Z:
        return s->kernel.block_dim[2];
    case GPGPU_REG_SHARED_MEM_SIZE:
        return s->kernel.shared_mem_size;
    case GPGPU_REG_DISPATCH:
        return 0;
    case GPGPU_REG_DMA_SRC_LO:
        return (uint32_t)s->dma.src_addr;
    case GPGPU_REG_DMA_SRC_HI:
        return (uint32_t)(s->dma.src_addr >> 32);
    case GPGPU_REG_DMA_DST_LO:
        return (uint32_t)s->dma.dst_addr;
    case GPGPU_REG_DMA_DST_HI:
        return (uint32_t)(s->dma.dst_addr >> 32);
    case GPGPU_REG_DMA_SIZE:
        return s->dma.size;
    case GPGPU_REG_DMA_CTRL:
        return s->dma.ctrl;
    case GPGPU_REG_DMA_STATUS:
        return s->dma.status;
    case GPGPU_REG_THREAD_ID_X:
        return s->simt.thread_id[0];
    case GPGPU_REG_THREAD_ID_Y:
        return s->simt.thread_id[1];
    case GPGPU_REG_THREAD_ID_Z:
        return s->simt.thread_id[2];
    case GPGPU_REG_BLOCK_ID_X:
        return s->simt.block_id[0];
    case GPGPU_REG_BLOCK_ID_Y:
        return s->simt.block_id[1];
    case GPGPU_REG_BLOCK_ID_Z:
        return s->simt.block_id[2];
    case GPGPU_REG_WARP_ID:
        return s->simt.warp_id;
    case GPGPU_REG_LANE_ID:
        return s->simt.lane_id;
    case GPGPU_REG_BARRIER:
        return s->simt.barrier_count;
    case GPGPU_REG_THREAD_MASK:
        return s->simt.thread_mask;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gpgpu: invalid ctrl read addr=0x%" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

/* TODO: Implement MMIO control register write */
static void gpgpu_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    (void)size;

    GPGPUState *s = opaque;
    uint32_t value = val;

    switch (addr) {
    case GPGPU_REG_GLOBAL_CTRL:
        s->global_ctrl = value & GPGPU_CTRL_ENABLE;
        if (value & GPGPU_CTRL_RESET) {
            s->global_status = GPGPU_STATUS_READY;
            s->error_status = 0;
            s->irq_status = 0;
            memset(&s->kernel, 0, sizeof(s->kernel));
            memset(&s->dma, 0, sizeof(s->dma));
            memset(&s->simt, 0, sizeof(s->simt));
            timer_del(s->dma_timer);
            timer_del(s->kernel_timer);
            if (s->vram_ptr) {
                memset(s->vram_ptr, 0, s->vram_size);
            }
        }
        break;

    case GPGPU_REG_ERROR_STATUS:
        s->error_status &= ~value;
        break;

    case GPGPU_REG_IRQ_ENABLE:
        s->irq_enable = value & (GPGPU_IRQ_KERNEL_DONE |
                                 GPGPU_IRQ_DMA_DONE |
                                 GPGPU_IRQ_ERROR);
        break;

    case GPGPU_REG_IRQ_ACK:
        s->irq_status &= ~value;
        gpgpu_update_intx(s);
        break;

    case GPGPU_REG_KERNEL_ADDR_LO:
        s->kernel.kernel_addr = (s->kernel.kernel_addr & 0xffffffff00000000ULL) |
                                value;
        break;

    case GPGPU_REG_KERNEL_ADDR_HI:
        s->kernel.kernel_addr = (s->kernel.kernel_addr & 0x00000000ffffffffULL) |
                                ((uint64_t)value << 32);
        break;

    case GPGPU_REG_KERNEL_ARGS_LO:
        s->kernel.kernel_args = (s->kernel.kernel_args & 0xffffffff00000000ULL) |
                                value;
        break;

    case GPGPU_REG_KERNEL_ARGS_HI:
        s->kernel.kernel_args = (s->kernel.kernel_args & 0x00000000ffffffffULL) |
                                ((uint64_t)value << 32);
        break;

    case GPGPU_REG_GRID_DIM_X:
        s->kernel.grid_dim[0] = value;
        break;

    case GPGPU_REG_GRID_DIM_Y:
        s->kernel.grid_dim[1] = value;
        break;

    case GPGPU_REG_GRID_DIM_Z:
        s->kernel.grid_dim[2] = value;
        break;

    case GPGPU_REG_BLOCK_DIM_X:
        s->kernel.block_dim[0] = value;
        break;

    case GPGPU_REG_BLOCK_DIM_Y:
        s->kernel.block_dim[1] = value;
        break;

    case GPGPU_REG_BLOCK_DIM_Z:
        s->kernel.block_dim[2] = value;
        break;

    case GPGPU_REG_SHARED_MEM_SIZE:
        s->kernel.shared_mem_size = value;
        break;

    case GPGPU_REG_DISPATCH:
        if (!(s->global_ctrl & GPGPU_CTRL_ENABLE) ||
            (s->global_status & GPGPU_STATUS_BUSY)) {
            s->error_status |= GPGPU_ERR_INVALID_CMD;
            s->global_status |= GPGPU_STATUS_ERROR;
            gpgpu_raise_irq(s, GPGPU_IRQ_ERROR, GPGPU_MSIX_VEC_ERROR);
            break;
        }

        s->global_status &= ~GPGPU_STATUS_READY;
        s->global_status |= GPGPU_STATUS_BUSY;
        gpgpu_kernel_complete(s);
        break;

    case GPGPU_REG_DMA_SRC_LO:
        s->dma.src_addr = (s->dma.src_addr & 0xffffffff00000000ULL) | value;
        break;

    case GPGPU_REG_DMA_SRC_HI:
        s->dma.src_addr = (s->dma.src_addr & 0x00000000ffffffffULL) |
                          ((uint64_t)value << 32);
        break;

    case GPGPU_REG_DMA_DST_LO:
        s->dma.dst_addr = (s->dma.dst_addr & 0xffffffff00000000ULL) | value;
        break;

    case GPGPU_REG_DMA_DST_HI:
        s->dma.dst_addr = (s->dma.dst_addr & 0x00000000ffffffffULL) |
                          ((uint64_t)value << 32);
        break;

    case GPGPU_REG_DMA_SIZE:
        s->dma.size = value;
        break;

    case GPGPU_REG_DMA_CTRL:
        s->dma.ctrl = value;
        if (value & GPGPU_DMA_START) {
            if (s->dma.status & GPGPU_DMA_BUSY) {
                s->error_status |= GPGPU_ERR_INVALID_CMD;
                s->global_status |= GPGPU_STATUS_ERROR;
                gpgpu_raise_irq(s, GPGPU_IRQ_ERROR, GPGPU_MSIX_VEC_ERROR);
                break;
            }

            s->dma.status = GPGPU_DMA_BUSY;
            gpgpu_dma_complete(s);
        }
        break;

    case GPGPU_REG_THREAD_ID_X:
        s->simt.thread_id[0] = value;
        break;

    case GPGPU_REG_THREAD_ID_Y:
        s->simt.thread_id[1] = value;
        break;

    case GPGPU_REG_THREAD_ID_Z:
        s->simt.thread_id[2] = value;
        break;

    case GPGPU_REG_BLOCK_ID_X:
        s->simt.block_id[0] = value;
        break;

    case GPGPU_REG_BLOCK_ID_Y:
        s->simt.block_id[1] = value;
        break;

    case GPGPU_REG_BLOCK_ID_Z:
        s->simt.block_id[2] = value;
        break;

    case GPGPU_REG_WARP_ID:
        s->simt.warp_id = value;
        break;

    case GPGPU_REG_LANE_ID:
        s->simt.lane_id = value & (GPGPU_WARP_SIZE - 1);
        break;

    case GPGPU_REG_THREAD_MASK:
        s->simt.thread_mask = value;
        break;

    case GPGPU_REG_BARRIER:
        s->simt.barrier_active = true;
        s->simt.barrier_count++;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gpgpu: invalid ctrl write addr=0x%" HWADDR_PRIx
                      " val=0x%" PRIx64 "\n",
                      addr, val);
        break;
    }
}

static const MemoryRegionOps gpgpu_ctrl_ops = {
    .read = gpgpu_ctrl_read,
    .write = gpgpu_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* TODO: Implement VRAM read */
static uint64_t gpgpu_vram_read(void *opaque, hwaddr addr, unsigned size)
{
    GPGPUState *s = opaque;

    if (addr > s->vram_size || size > s->vram_size - addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gpgpu: vram read out of bounds addr=0x%" HWADDR_PRIx
                      " size=%u\n",
                      addr, size);
        s->error_status |= GPGPU_ERR_VRAM_FAULT;
        s->global_status |= GPGPU_STATUS_ERROR;
        gpgpu_raise_irq(s, GPGPU_IRQ_ERROR, GPGPU_MSIX_VEC_ERROR);
        return 0;
    }

    return ldn_le_p(s->vram_ptr + addr, size);
}

/* TODO: Implement VRAM write */
static void gpgpu_vram_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    GPGPUState *s = opaque;

    if (addr > s->vram_size || size > s->vram_size - addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gpgpu: vram write out of bounds addr=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 "\n",
                      addr, size, val);
        s->error_status |= GPGPU_ERR_VRAM_FAULT;
        s->global_status |= GPGPU_STATUS_ERROR;
        gpgpu_raise_irq(s, GPGPU_IRQ_ERROR, GPGPU_MSIX_VEC_ERROR);
        return;
    }

    stn_le_p(s->vram_ptr + addr, size, val);
}

static const MemoryRegionOps gpgpu_vram_ops = {
    .read = gpgpu_vram_read,
    .write = gpgpu_vram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static uint64_t gpgpu_doorbell_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)size;
    return 0;
}

static void gpgpu_doorbell_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)val;
    (void)size;
}

static const MemoryRegionOps gpgpu_doorbell_ops = {
    .read = gpgpu_doorbell_read,
    .write = gpgpu_doorbell_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void gpgpu_update_intx(GPGPUState *s)
{
    PCIDevice *pdev = &s->parent_obj;

    if (!msix_enabled(pdev) && !msi_enabled(pdev)) {
        pci_set_irq(pdev, !!(s->irq_status & s->irq_enable));
    }
}

static void gpgpu_raise_irq(GPGPUState *s, uint32_t irq, unsigned vector)
{
    PCIDevice *pdev = &s->parent_obj;

    s->irq_status |= irq;
    if (!(s->irq_enable & irq)) {
        return;
    }

    if (msix_enabled(pdev)) {
        msix_notify(pdev, vector);
    } else if (msi_enabled(pdev)) {
        msi_notify(pdev, 0);
    } else {
        gpgpu_update_intx(s);
    }
}

/* TODO: Implement DMA completion handler */
static void gpgpu_dma_complete(void *opaque)
{
    GPGPUState *s = opaque;
    bool from_vram = s->dma.ctrl & GPGPU_DMA_DIR_FROM_VRAM;
    uint64_t vram_addr = from_vram ? s->dma.src_addr : s->dma.dst_addr;
    MemTxResult ret;

    s->dma.status &= ~GPGPU_DMA_BUSY;

    if (vram_addr > s->vram_size || s->dma.size > s->vram_size - vram_addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gpgpu: dma vram out of bounds addr=0x%" PRIx64
                      " size=%u\n",
                      vram_addr, s->dma.size);
        s->dma.status = GPGPU_DMA_ERROR;
        s->error_status |= GPGPU_ERR_DMA_FAULT;
        s->global_status |= GPGPU_STATUS_ERROR;
        gpgpu_raise_irq(s, GPGPU_IRQ_ERROR, GPGPU_MSIX_VEC_ERROR);
        return;
    }

    if (from_vram) {
        ret = pci_dma_write(&s->parent_obj, s->dma.dst_addr,
                            s->vram_ptr + vram_addr, s->dma.size);
    } else {
        ret = pci_dma_read(&s->parent_obj, s->dma.src_addr,
                           s->vram_ptr + vram_addr, s->dma.size);
    }

    if (ret != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gpgpu: dma transfer failed src=0x%" PRIx64
                      " dst=0x%" PRIx64 " size=%u\n",
                      s->dma.src_addr, s->dma.dst_addr, s->dma.size);
        s->dma.status = GPGPU_DMA_ERROR;
        s->error_status |= GPGPU_ERR_DMA_FAULT;
        s->global_status |= GPGPU_STATUS_ERROR;
        gpgpu_raise_irq(s, GPGPU_IRQ_ERROR, GPGPU_MSIX_VEC_ERROR);
        return;
    }

    s->dma.ctrl &= ~GPGPU_DMA_START;
    s->dma.status = GPGPU_DMA_COMPLETE;
    if (s->dma.ctrl & GPGPU_DMA_IRQ_ENABLE) {
        gpgpu_raise_irq(s, GPGPU_IRQ_DMA_DONE, GPGPU_MSIX_VEC_DMA);
    }
}

/* TODO: Implement kernel completion handler */
static void gpgpu_kernel_complete(void *opaque)
{
    GPGPUState *s = opaque;

    s->global_status &= ~GPGPU_STATUS_BUSY;

    if (gpgpu_core_exec_kernel(s) < 0) {
        s->error_status |= GPGPU_ERR_KERNEL_FAULT;
        s->global_status |= GPGPU_STATUS_ERROR;
        gpgpu_raise_irq(s, GPGPU_IRQ_ERROR, GPGPU_MSIX_VEC_ERROR);
        return;
    }

    s->global_status |= GPGPU_STATUS_READY;
    gpgpu_raise_irq(s, GPGPU_IRQ_KERNEL_DONE, GPGPU_MSIX_VEC_KERNEL);
}

static void gpgpu_realize(PCIDevice *pdev, Error **errp)
{
    GPGPUState *s = GPGPU(pdev);
    uint8_t *pci_conf = pdev->config;

    pci_config_set_interrupt_pin(pci_conf, 1);

    s->vram_ptr = g_malloc0(s->vram_size);
    if (!s->vram_ptr) {
        error_setg(errp, "GPGPU: failed to allocate VRAM");
        return;
    }

    /* BAR 0: control registers */
    memory_region_init_io(&s->ctrl_mmio, OBJECT(s), &gpgpu_ctrl_ops, s,
                          "gpgpu-ctrl", GPGPU_CTRL_BAR_SIZE);
    pci_register_bar(pdev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->ctrl_mmio);

    /* BAR 2: VRAM */
    memory_region_init_io(&s->vram, OBJECT(s), &gpgpu_vram_ops, s,
                          "gpgpu-vram", s->vram_size);
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->vram);

    /* BAR 4: doorbell registers */
    memory_region_init_io(&s->doorbell_mmio, OBJECT(s), &gpgpu_doorbell_ops, s,
                          "gpgpu-doorbell", GPGPU_DOORBELL_BAR_SIZE);
    pci_register_bar(pdev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->doorbell_mmio);

    if (msix_init(pdev, GPGPU_MSIX_VECTORS,
                  &s->ctrl_mmio, 0, 0xFE000,
                  &s->ctrl_mmio, 0, 0xFF000,
                  0, errp)) {
        g_free(s->vram_ptr);
        return;
    }

    msi_init(pdev, 0, 1, true, false, errp);

    s->dma_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, gpgpu_dma_complete, s);
    s->kernel_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                   gpgpu_kernel_complete, s);

    s->global_status = GPGPU_STATUS_READY;
}

static void gpgpu_exit(PCIDevice *pdev)
{
    GPGPUState *s = GPGPU(pdev);

    timer_free(s->dma_timer);
    timer_free(s->kernel_timer);
    g_free(s->vram_ptr);
    msix_uninit(pdev, &s->ctrl_mmio, &s->ctrl_mmio);
    msi_uninit(pdev);
}

static void gpgpu_reset(DeviceState *dev)
{
    GPGPUState *s = GPGPU(dev);

    s->global_ctrl = 0;
    s->global_status = GPGPU_STATUS_READY;
    s->error_status = 0;
    s->irq_enable = 0;
    s->irq_status = 0;
    memset(&s->kernel, 0, sizeof(s->kernel));
    memset(&s->dma, 0, sizeof(s->dma));
    memset(&s->simt, 0, sizeof(s->simt));
    timer_del(s->dma_timer);
    timer_del(s->kernel_timer);
    if (s->vram_ptr) {
        memset(s->vram_ptr, 0, s->vram_size);
    }
}

static const Property gpgpu_properties[] = {
    DEFINE_PROP_UINT32("num_cus", GPGPUState, num_cus,
                       GPGPU_DEFAULT_NUM_CUS),
    DEFINE_PROP_UINT32("warps_per_cu", GPGPUState, warps_per_cu,
                       GPGPU_DEFAULT_WARPS_PER_CU),
    DEFINE_PROP_UINT32("warp_size", GPGPUState, warp_size,
                       GPGPU_DEFAULT_WARP_SIZE),
    DEFINE_PROP_UINT64("vram_size", GPGPUState, vram_size,
                       GPGPU_DEFAULT_VRAM_SIZE),
};

static const VMStateDescription vmstate_gpgpu = {
    .name = "gpgpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, GPGPUState),
        VMSTATE_UINT32(global_ctrl, GPGPUState),
        VMSTATE_UINT32(global_status, GPGPUState),
        VMSTATE_UINT32(error_status, GPGPUState),
        VMSTATE_UINT32(irq_enable, GPGPUState),
        VMSTATE_UINT32(irq_status, GPGPUState),
        VMSTATE_END_OF_LIST()
    }
};

static void gpgpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = gpgpu_realize;
    pc->exit = gpgpu_exit;
    pc->vendor_id = GPGPU_VENDOR_ID;
    pc->device_id = GPGPU_DEVICE_ID;
    pc->revision = GPGPU_REVISION;
    pc->class_id = GPGPU_CLASS_CODE;

    device_class_set_legacy_reset(dc, gpgpu_reset);
    dc->desc = "Educational GPGPU Device";
    dc->vmsd = &vmstate_gpgpu;
    device_class_set_props(dc, gpgpu_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo gpgpu_type_info = {
    .name          = TYPE_GPGPU,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GPGPUState),
    .class_init    = gpgpu_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void gpgpu_register_types(void)
{
    type_register_static(&gpgpu_type_info);
}

type_init(gpgpu_register_types)
