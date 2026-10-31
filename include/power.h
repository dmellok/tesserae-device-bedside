/*
 * power.h: the PowerBoost 1000C's sense lines (docs/wiring-guide.html), each
 * through a 100 k / 100 k divider:
 *   GP26 (ADC0)  Bat: the LiPo, 3.0-4.2 V
 *   GP27 (ADC1)  USB: ~5 V while mains is in, 0 V on battery
 *   GP28 (ADC2)  LBO: battery voltage until it drops below ~3.2 V, then 0 V
 * Read only when the panel's "Battery backup" setting says it is fitted:
 * unconnected ADC pins float and would show a battery that isn't there.
 */
#pragma once

#include <stdbool.h>

void  power_init(void);
/* Every ~1 s from the main loop; true when mains or the battery level changed. */
bool  power_tick(void);

bool  power_on_battery(void);   /* no mains on the USB-C input */
bool  power_low(void);          /* LBO tripped, or the battery under ~3.4 V on battery */
int   power_percent(void);      /* LiPo charge estimate from its voltage, 0..100 */
float power_battery_v(void);
float power_usb_v(void);
