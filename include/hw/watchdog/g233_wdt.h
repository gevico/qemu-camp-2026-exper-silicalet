#ifndef HW_G233_WDT_H
#define HW_G233_WDT_H

#include "hw/core/sysbus.h"

#define TYPE_G233_WDT "g233-wdt"

DeviceState *g233_wdt_create(hwaddr addr, qemu_irq irq);

#endif
