/*
 * psram.h for the simulator: the 8 MB PSRAM is an ordinary array, so the
 * firmware's fixed offsets into it (frame buffers, the log, the audio ring)
 * work unchanged. Replaces include/psram.h in the build's header mirror.
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "pico/stdlib.h"

#define PSRAM_CS_PIN_PLUS2   47u
#define SIM_PSRAM_BYTES      (8u * 1024 * 1024)
extern uint8_t sim_psram[SIM_PSRAM_BYTES];
#define PSRAM_XIP_BASE       (sim_psram)
#define PSRAM_XIP_NOCACHE    (sim_psram)

size_t psram_init(uint cs_pin);
bool psram_test(size_t size_bytes);
