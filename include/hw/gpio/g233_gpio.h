#ifndef HW_G233_GPIO_H
#define HW_G233_GPIO_H

#include "hw/core/sysbus.h"

#define TYPE_G233_GPIO "g233-gpio"

DeviceState *g233_gpio_create(hwaddr addr, qemu_irq irq);

#endif
