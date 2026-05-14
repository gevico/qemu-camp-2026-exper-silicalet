#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/g233_spi.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define CR1_SPE BIT(0)
#define CR1_MSTR BIT(2)
#define CR1_ERRIE BIT(5)
#define CR1_RXNEIE BIT(6)
#define CR1_TXEIE BIT(7)

#define SR_RXNE BIT(0)
#define SR_TXE BIT(1)
#define SR_OVERRUN BIT(4)

typedef struct G233SPIState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq cs[2];
    SSIBus *spi;
    uint32_t cr1;
    uint32_t cr2;
    uint32_t sr;
    uint32_t dr;
} G233SPIState;

OBJECT_DECLARE_SIMPLE_TYPE(G233SPIState, G233_SPI)

static void g233_spi_update_cs(G233SPIState *s)
{
    qemu_set_irq(s->cs[0], (s->cr2 & 3) != 0);
    qemu_set_irq(s->cs[1], (s->cr2 & 3) != 1);
}

static void g233_spi_update_irq(G233SPIState *s)
{
    bool level = false;

    if ((s->cr1 & CR1_TXEIE) && (s->sr & SR_TXE)) {
        level = true;
    }
    if ((s->cr1 & CR1_RXNEIE) && (s->sr & SR_RXNE)) {
        level = true;
    }
    if ((s->cr1 & CR1_ERRIE) && (s->sr & SR_OVERRUN)) {
        level = true;
    }
    qemu_set_irq(s->irq, level);
}

static void g233_spi_xfer(G233SPIState *s, uint8_t tx)
{
    uint32_t rx;

    if ((s->cr1 & (CR1_SPE | CR1_MSTR)) != (CR1_SPE | CR1_MSTR)) {
        return;
    }
    if (s->sr & SR_RXNE) {
        s->sr |= SR_OVERRUN;
    }
    g233_spi_update_cs(s);
    rx = ssi_transfer(s->spi, tx);
    s->dr = rx & 0xff;
    s->sr |= SR_RXNE | SR_TXE;
    g233_spi_update_irq(s);
}

static uint64_t g233_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    G233SPIState *s = opaque;

    switch (addr) {
    case 0x00:
        return s->cr1;
    case 0x04:
        return s->cr2;
    case 0x08:
        return s->sr;
    case 0x0c: {
        uint32_t ret = s->dr;
        s->sr &= ~SR_RXNE;
        g233_spi_update_irq(s);
        return ret;
    }
    default:
        return 0;
    }
}

static void g233_spi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    G233SPIState *s = opaque;
    uint32_t x = val;

    switch (addr) {
    case 0x00:
        s->cr1 = x & (CR1_SPE | CR1_MSTR | CR1_ERRIE | CR1_RXNEIE | CR1_TXEIE);
        break;
    case 0x04:
        s->cr2 = x & 3;
        g233_spi_update_cs(s);
        break;
    case 0x08:
        s->sr &= ~(x & SR_OVERRUN);
        break;
    case 0x0c:
        g233_spi_xfer(s, x & 0xff);
        return;
    default:
        return;
    }
    g233_spi_update_irq(s);
}

static const MemoryRegionOps g233_spi_ops = {
    .read = g233_spi_read,
    .write = g233_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const VMStateDescription vmstate_g233_spi = {
    .name = TYPE_G233_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr1, G233SPIState),
        VMSTATE_UINT32(cr2, G233SPIState),
        VMSTATE_UINT32(sr, G233SPIState),
        VMSTATE_UINT32(dr, G233SPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_spi_reset(DeviceState *dev)
{
    G233SPIState *s = G233_SPI(dev);

    s->cr1 = 0;
    s->cr2 = 0;
    s->sr = SR_TXE;
    s->dr = 0;
    g233_spi_update_cs(s);
    g233_spi_update_irq(s);
}

static void g233_spi_init(Object *obj)
{
    G233SPIState *s = G233_SPI(obj);
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &g233_spi_ops, s, TYPE_G233_SPI, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->cs[0]);
    sysbus_init_irq(sbd, &s->cs[1]);
    s->spi = ssi_create_bus(dev, "spi");
}

static void g233_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_g233_spi;
    device_class_set_legacy_reset(dc, g233_spi_reset);
}

static const TypeInfo g233_spi_info = {
    .name = TYPE_G233_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233SPIState),
    .instance_init = g233_spi_init,
    .class_init = g233_spi_class_init,
};

static void g233_spi_register_types(void)
{
    type_register_static(&g233_spi_info);
}

type_init(g233_spi_register_types)

DeviceState *g233_spi_create(hwaddr addr, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_G233_SPI);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(s, &error_fatal);
    sysbus_mmio_map(s, 0, addr);
    sysbus_connect_irq(s, 0, irq);
    return dev;
}

#define TYPE_G233_RUST_SPI "g233-rust-spi"

#define RUST_CR1_SPE BIT(0)
#define RUST_CR1_MSTR BIT(2)

#define RUST_SR_RXNE BIT(0)
#define RUST_SR_TXE BIT(1)
#define RUST_SR_OVERRUN BIT(4)

#define AT25_SR_WIP BIT(0)
#define AT25_SR_WEL BIT(1)

#define AT25_CMD_READ 0x03
#define AT25_CMD_RDSR 0x05
#define AT25_CMD_WREN 0x06
#define AT25_CMD_WRITE 0x02

typedef enum G233RustSPIPhase {
    G233_RUST_SPI_IDLE,
    G233_RUST_SPI_RDSR,
    G233_RUST_SPI_READ_ADDR,
    G233_RUST_SPI_READ_DATA,
    G233_RUST_SPI_WRITE_ADDR,
    G233_RUST_SPI_WRITE_DATA,
} G233RustSPIPhase;

typedef struct G233RustSPIState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t cr1;
    uint32_t sr;
    uint32_t dr;
    uint32_t cs;
    uint8_t flash[256];
    uint8_t flash_sr;
    uint8_t flash_addr;
    bool write_seen_data;
    int32_t phase;
} G233RustSPIState;

OBJECT_DECLARE_SIMPLE_TYPE(G233RustSPIState, G233_RUST_SPI)

static bool g233_rust_spi_is_cmd(uint8_t tx)
{
    switch (tx) {
    case AT25_CMD_READ:
    case AT25_CMD_RDSR:
    case AT25_CMD_WREN:
    case AT25_CMD_WRITE:
        return true;
    default:
        return false;
    }
}

static uint8_t g233_rust_spi_xfer(G233RustSPIState *s, uint8_t tx)
{
retry:
    switch (s->phase) {
    case G233_RUST_SPI_IDLE:
        switch (tx) {
        case AT25_CMD_WREN:
            s->flash_sr |= AT25_SR_WEL;
            return 0;
        case AT25_CMD_RDSR:
            s->phase = G233_RUST_SPI_RDSR;
            return 0;
        case AT25_CMD_READ:
            s->phase = G233_RUST_SPI_READ_ADDR;
            return 0;
        case AT25_CMD_WRITE:
            if (s->flash_sr & AT25_SR_WEL) {
                s->phase = G233_RUST_SPI_WRITE_ADDR;
            }
            return 0;
        default:
            return 0;
        }

    case G233_RUST_SPI_RDSR:
        s->phase = G233_RUST_SPI_IDLE;
        return s->flash_sr;

    case G233_RUST_SPI_READ_ADDR:
        s->flash_addr = tx;
        s->phase = G233_RUST_SPI_READ_DATA;
        return 0;

    case G233_RUST_SPI_READ_DATA:
        if (g233_rust_spi_is_cmd(tx)) {
            s->phase = G233_RUST_SPI_IDLE;
            goto retry;
        }
        return s->flash[s->flash_addr++];

    case G233_RUST_SPI_WRITE_ADDR:
        s->flash_addr = tx;
        s->write_seen_data = false;
        s->phase = G233_RUST_SPI_WRITE_DATA;
        return 0;

    case G233_RUST_SPI_WRITE_DATA:
        if (s->write_seen_data && g233_rust_spi_is_cmd(tx)) {
            s->flash_sr &= ~AT25_SR_WEL;
            s->phase = G233_RUST_SPI_IDLE;
            goto retry;
        }
        s->flash[s->flash_addr++] = tx;
        s->write_seen_data = true;
        return 0;
    }

    return 0;
}

static uint64_t g233_rust_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    G233RustSPIState *s = opaque;

    switch (addr) {
    case 0x00:
        return s->cr1;
    case 0x04:
        return s->sr;
    case 0x08: {
        uint32_t ret = s->dr;
        s->sr &= ~RUST_SR_RXNE;
        return ret;
    }
    case 0x0c:
        return s->cs;
    default:
        return 0;
    }
}

static void g233_rust_spi_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    G233RustSPIState *s = opaque;
    uint32_t x = val;

    switch (addr) {
    case 0x00:
        s->cr1 = x & (RUST_CR1_SPE | RUST_CR1_MSTR);
        if ((s->cr1 & (RUST_CR1_SPE | RUST_CR1_MSTR)) ==
            (RUST_CR1_SPE | RUST_CR1_MSTR)) {
            s->sr |= RUST_SR_TXE;
        } else {
            s->sr = 0;
        }
        break;
    case 0x08:
        if ((s->cr1 & (RUST_CR1_SPE | RUST_CR1_MSTR)) !=
            (RUST_CR1_SPE | RUST_CR1_MSTR)) {
            return;
        }
        if (s->sr & RUST_SR_RXNE) {
            s->sr |= RUST_SR_OVERRUN;
        }
        s->dr = g233_rust_spi_xfer(s, x & 0xff);
        s->sr |= RUST_SR_RXNE | RUST_SR_TXE;
        break;
    case 0x0c:
        s->cs = x & 1;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps g233_rust_spi_ops = {
    .read = g233_rust_spi_read,
    .write = g233_rust_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const VMStateDescription vmstate_g233_rust_spi = {
    .name = TYPE_G233_RUST_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr1, G233RustSPIState),
        VMSTATE_UINT32(sr, G233RustSPIState),
        VMSTATE_UINT32(dr, G233RustSPIState),
        VMSTATE_UINT32(cs, G233RustSPIState),
        VMSTATE_UINT8_ARRAY(flash, G233RustSPIState, 256),
        VMSTATE_UINT8(flash_sr, G233RustSPIState),
        VMSTATE_UINT8(flash_addr, G233RustSPIState),
        VMSTATE_BOOL(write_seen_data, G233RustSPIState),
        VMSTATE_INT32(phase, G233RustSPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_rust_spi_reset(DeviceState *dev)
{
    G233RustSPIState *s = G233_RUST_SPI(dev);

    memset(s->flash, 0xff, sizeof(s->flash));
    s->cr1 = 0;
    s->sr = 0;
    s->dr = 0;
    s->cs = 0;
    s->flash_sr = 0;
    s->flash_addr = 0;
    s->write_seen_data = false;
    s->phase = G233_RUST_SPI_IDLE;
    qemu_set_irq(s->irq, 0);
}

static void g233_rust_spi_init(Object *obj)
{
    G233RustSPIState *s = G233_RUST_SPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &g233_rust_spi_ops, s,
                          TYPE_G233_RUST_SPI, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
}

static void g233_rust_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_g233_rust_spi;
    device_class_set_legacy_reset(dc, g233_rust_spi_reset);
}

static const TypeInfo g233_rust_spi_info = {
    .name = TYPE_G233_RUST_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233RustSPIState),
    .instance_init = g233_rust_spi_init,
    .class_init = g233_rust_spi_class_init,
};

static void g233_rust_spi_register_types(void)
{
    type_register_static(&g233_rust_spi_info);
}

type_init(g233_rust_spi_register_types)

DeviceState *g233_rust_spi_create(hwaddr addr, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_G233_RUST_SPI);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(s, &error_fatal);
    sysbus_mmio_map(s, 0, addr);
    sysbus_connect_irq(s, 0, irq);
    return dev;
}
