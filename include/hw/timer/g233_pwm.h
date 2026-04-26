#ifndef HW_G233_PWM_H
#define HW_G233_PWM_H

#include "hw/core/sysbus.h"

#define TYPE_G233_PWM "g233-pwm"

DeviceState *g233_pwm_create(hwaddr addr);

#endif
