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
