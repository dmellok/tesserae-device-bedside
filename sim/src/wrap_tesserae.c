/*
 * wrap_tesserae.c: src/tesserae.c as it is, with the time zone from the Mac
 * until a server has said which one it is.
 *
 * On the device the zone (Settings > Time zone: Automatic) comes from the
 * server's status reply; with no server it is UTC. In the simulator the Mac's
 * zone stands in, so the clock reads the same as the one on your desk.
 */
#define tesserae_tz_offset_s tesserae_tz_offset_s_device
#include "tesserae.c"
#undef tesserae_tz_offset_s
#include "sim.h"

int32_t tesserae_tz_offset_s(void)
{
    if (s_tz_known || s_local_time[0]) return tesserae_tz_offset_s_device();
    return sim_host_tz_offset_s();
}
