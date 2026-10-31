/*
 * menu.h: the one-line status banner in the bottom-left corner (web UI URL and
 * pairing state while the device is not yet showing a dashboard). The menu
 * sheet and the pages live in local.h.
 */
#pragma once

#include <stdint.h>
#include "it8951.h"

void banner_rect(const char *text, int *x, int *y, int *w, int *h);
void banner_overlay_row(const char *text, int y, uint8_t row[IT8951_ROW_BYTES]);
