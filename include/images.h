/*
 * images.h: embedded test card.
 *
 * `sample_card` is a landscape 1872x1404 16-grey packed-4bpp frame
 * (IT8951_FRAME_BYTES) embedded from src/sample_card.bin via src/sample_card.S.
 * It is what the panel shows before the first Tesserae frame arrives, and the
 * orientation check: its black corner square must appear top-LEFT (otherwise
 * flip EPD_MIRROR_H / the web UI's mirror box). Regenerate with
 * tools/gen_sample.py or from your own PNG with tools/png2frame.py.
 */
#pragma once

#include <stdint.h>

extern const uint8_t sample_card[];
