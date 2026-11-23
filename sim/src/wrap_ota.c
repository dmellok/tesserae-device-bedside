/*
 * wrap_ota.c: src/ota.c as it is (the GitHub release list, the download into
 * slot B, the checksum), with the boot ROM's calls from sim_sdk.c.
 *
 * One line of it can't build for the Mac: an Arm barrier after the boot ROM's
 * "buy" (asm "dsb; isb"). Every header ota.c uses is included first, then the
 * asm statement is turned into the host's full barrier for ota.c alone.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ota.h"
#include "sha256.h"
#include "net_rest.h"
#include "config.h"
#include "cJSON.h"
#include "pico/stdlib.h"

#define __asm
#define volatile(...) __sync_synchronize()   /* only "volatile(" : the asm; "volatile uint32_t" is untouched */
#include "ota.c"
#undef volatile
#undef __asm
