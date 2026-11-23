/*
 * wrap_net_rest.c: src/net_rest.c as it is, plus one hook beside its statics.
 *
 * The device has no wall clock until a server answers (the Date header of a
 * Tesserae reply). The simulator starts the clock from the Mac's (plus any
 * time warp) so the panel shows the time with no server; the first reply's
 * Date header takes over as on the device.
 */
#include <time.h>
#include "net_rest.c"
#include "sim.h"

void sim_seed_epoch(void)
{
    if (s_epoch_at_ref) return;
    s_epoch_at_ref = (uint32_t)time(NULL) + (uint32_t)sim_warp_total_s();
    s_epoch_ref_ms = now_ms();
}
