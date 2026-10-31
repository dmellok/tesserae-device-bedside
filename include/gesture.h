/*
 * gesture.h: the APDS-9960 gesture sensor behind the top bezel, on the Qw/ST
 * I2C chain with the knob and keys (0x39). Polled, like them; its INT pin is
 * not used.
 *
 * The sensor's own gesture engine wakes when a hand comes within a few cm and
 * fills a FIFO with four photodiode readings (up, down, left, right) per
 * frame; the direction comes from how the balance between opposite diodes
 * moves from the first frame to the last (SparkFun's method).
 */
#pragma once

#include <stdbool.h>

typedef enum { GEST_NONE, GEST_LEFT, GEST_RIGHT, GEST_UP, GEST_DOWN, GEST_WAVE } gesture_t;

bool        gesture_init(void);       /* after the I2C bus is up (inputs_init); false if no sensor */
bool        gesture_present(void);
void        gesture_enable(bool on);  /* off: the sensor sleeps (and draws ~nothing) */
gesture_t   gesture_poll(void);       /* every main-loop pass; cheap when idle */
const char *gesture_name(gesture_t g);
