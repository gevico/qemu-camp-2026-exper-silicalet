#ifndef HW_G233_SPI_H
#define HW_G233_SPI_H

#include "hw/core/sysbus.h"

#define TYPE_G233_SPI "g233-spi"

DeviceState *g233_spi_create(hwaddr addr, qemu_irq irq);

#endif
