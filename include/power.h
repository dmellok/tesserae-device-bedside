/*
 * power.h: the PowerBoost 1000C's sense lines (docs/wiring-guide.html), each
 * through a 100 k / 100 k divider:
 *   GP26/GP40 (ADC0)  Bat: the LiPo, 3.0-4.2 V
 *   GP27/GP41 (ADC1)  USB: ~5 V while mains is in, 0 V on battery
 *   GP28/GP42 (ADC2)  LBO: battery voltage until the cell drops below ~3.2 V, then
 *                0 V (trusted once it has read like a battery; optional)
 * The Pico Plus 2 W ties each GP26-28 pad to GP40-42, the RP2350B's ADC pins.
 * Read only when the panel's "Battery backup" setting says it is fitted:
 * unconnected ADC pins float and would show a battery that isn't there.
 */
#pragma once

#include <stdbool.h>

void  power_init(void);
/* Every ~1 s from the main loop; true when mains or the battery level changed. */
bool  power_tick(void);

bool  power_on_battery(void);   /* no mains on the USB-C input */
bool  power_low(void);          /* on battery and LBO tripped or the cell under ~3.4 V */
int   power_percent(void);      /* LiPo charge estimate from its voltage, 0..100 */
float power_battery_v(void);
float power_usb_v(void);
/* Diagnostics: millivolts at the pin (no divider) on ADC channel 0..7, GP40..47. */
int   power_adc_pin_mv(int ch);
