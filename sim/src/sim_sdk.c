/*
 * sim_sdk.c: the Pico SDK on the host (see include/sim_sdk.h): the clock and
 * its time warp, sleeping, stdio, the second core, the flash file, the
 * watchdog and boot ROM, the board id.
 */
#include "sim.h"
#include "sim_lwip.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

sim_opts_t sim_opt = { .port = 8080 };

/* ---- the clock ----------------------------------------------------------------- */

static uint64_t s_t0_us;              /* the host's monotonic clock at boot */
static volatile int64_t s_warp_us;    /* jumped this boot */
static int64_t s_warp_before_s;       /* jumped in earlier boots (SIM_WARP_S across the restart) */
static pthread_t s_main_thread;

static uint64_t host_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

__attribute__((constructor)) static void clock_start(void)
{
    s_t0_us = host_us();
    s_main_thread = pthread_self();
    const char *w = getenv("SIM_WARP_S");
    if (w) s_warp_before_s = strtoll(w, NULL, 10);
}

bool     sim_is_main_thread(void) { return pthread_equal(pthread_self(), s_main_thread); }
uint64_t sim_real_us(void)        { return host_us() - s_t0_us; }
uint64_t time_us_64(void)         { return sim_real_us() + (uint64_t)s_warp_us; }
int64_t  sim_warp_total_s(void)   { return s_warp_before_s + s_warp_us / 1000000; }

void sim_warp(int64_t seconds)
{
    if (seconds <= 0) return;
    s_warp_us += seconds * 1000000;
    printf("sim: time warp +%lld s (total %+lld s)\n", (long long)seconds, (long long)sim_warp_total_s());
}

int32_t sim_host_tz_offset_s(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (int32_t)lt.tm_gmtoff;
}

void sleep_us(uint64_t us)
{
    if (!us) return;
    if (sim_is_main_thread()) {           /* the firmware waits here for the network: answer it */
        uint64_t until = host_us() + us;
        for (;;) {
            uint64_t now = host_us();
            if (now >= until) break;
            uint64_t left = until - now;
            sim_lwip_pump((uint32_t)((left + 999) / 1000));
            if (left < 1000) { usleep((useconds_t)left); break; }
        }
        return;
    }
    usleep((useconds_t)us);
}
void sleep_ms(uint32_t ms) { sleep_us((uint64_t)ms * 1000u); }
void sim_wfe(void)         { usleep(200); }

alarm_id_t add_alarm_in_ms(uint32_t ms, alarm_callback_t cb, void *user_data, bool fire_if_past)
{
    (void)ms; (void)cb; (void)user_data; (void)fire_if_past;
    return 1;          /* only the USB-flash fail-safe uses one; the simulator never needs it */
}
bool cancel_alarm(alarm_id_t id) { (void)id; return true; }

/* ---- stdio --------------------------------------------------------------------- */

#define MAX_DRIVERS 4
static stdio_driver_t *s_drivers[MAX_DRIVERS];
static pthread_mutex_t s_out_lock = PTHREAD_MUTEX_INITIALIZER;

bool stdio_init_all(void) { setvbuf(stdout, NULL, _IOLBF, 0); return true; }
void stdio_flush(void)    { fflush(stdout); }

void stdio_set_driver_enabled(stdio_driver_t *d, bool on)
{
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (on && !s_drivers[i]) { s_drivers[i] = d; return; }
        if (!on && s_drivers[i] == d) { s_drivers[i] = NULL; return; }
    }
}

static void out(const char *b, int n)
{
    if (n <= 0) return;
    pthread_mutex_lock(&s_out_lock);
    fwrite(b, 1, (size_t)n, stdout);
    for (int i = 0; i < MAX_DRIVERS; i++) if (s_drivers[i] && s_drivers[i]->out_chars) s_drivers[i]->out_chars(b, n);
    pthread_mutex_unlock(&s_out_lock);
}

int sim_printf(const char *fmt, ...)
{
    char small[1024], *b = small;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof small) {
        b = malloc((size_t)n + 1);
        if (!b) return 0;
        va_start(ap, fmt);
        vsnprintf(b, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    out(b, n);
    if (b != small) free(b);
    return n;
}
int sim_putchar(int c)       { char ch = (char)c; out(&ch, 1); return c; }
int sim_puts(const char *s)  { out(s, (int)strlen(s)); out("\n", 1); return 1; }

/* The terminal is the USB serial console: single keys reach handle_key(). */
int getchar_timeout_us(uint32_t timeout_us)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    if (!isatty(0) || poll(&p, 1, (int)(timeout_us / 1000u)) <= 0 || !(p.revents & POLLIN)) return PICO_ERROR_TIMEOUT;
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : PICO_ERROR_TIMEOUT;
}

/* ---- GPIO: the BOOT button can be pressed from the simulator page ----------------- */

static volatile bool s_boot_down;
void sim_boot_button(bool down) { s_boot_down = down; }
bool sim_gpio_get(uint pin)     { return pin == 45 ? !s_boot_down : true; }   /* GP45: BOOT, active low */

static ioqspi_hw_t s_ioqspi;
static sio_hw_t    s_sio = { .gpio_hi_in = 0xFFFFFFFFu };
ioqspi_hw_t *ioqspi_hw = &s_ioqspi;
sio_hw_t    *sio_hw    = &s_sio;

/* ---- the second core ---------------------------------------------------------------- */

static void *core1_thread(void *arg)
{
    pthread_setname_np("core1");
    ((void (*)(void))arg)();
    return NULL;
}

void multicore_launch_core1(void (*entry)(void))
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, core1_thread, (void *)entry);
    pthread_attr_destroy(&a);
}
void multicore_launch_core1_with_stack(void (*entry)(void), uint32_t *stack, size_t bytes) { (void)stack; (void)bytes; multicore_launch_core1(entry); }
void multicore_reset_core1(void) {}

/* ---- flash: a file, mapped -------------------------------------------------------- */

uint8_t *sim_flash;

void sim_flash_open(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { fprintf(stderr, "sim: can't open the flash file %s: %s\n", path, strerror(errno)); exit(1); }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size != (off_t)SIM_FLASH_BYTES) {               /* a new chip reads all ones */
        static uint8_t ff[65536];
        memset(ff, 0xFF, sizeof ff);
        ftruncate(fd, 0);
        for (uint32_t i = 0; i < SIM_FLASH_BYTES; i += sizeof ff) write(fd, ff, sizeof ff);
        fprintf(stderr, "sim: new flash image %s (16 MB, erased)\n", path);
    }
    sim_flash = mmap(NULL, SIM_FLASH_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (sim_flash == MAP_FAILED) { fprintf(stderr, "sim: can't map %s\n", path); exit(1); }
}
void sim_flash_sync(void) { if (sim_flash) msync(sim_flash, SIM_FLASH_BYTES, MS_SYNC); }

void flash_range_erase(uint32_t off, size_t n)
{
    if ((off % FLASH_SECTOR_SIZE) || (n % FLASH_SECTOR_SIZE) || off + n > SIM_FLASH_BYTES) {
        printf("sim: flash_range_erase(0x%06x, %zu) is not sector aligned\n", off, n); return;
    }
    memset(sim_flash + off, 0xFF, n);
}

void flash_range_program(uint32_t off, const uint8_t *data, size_t n)
{
    if ((off % FLASH_PAGE_SIZE) || (n % FLASH_PAGE_SIZE) || off + n > SIM_FLASH_BYTES) {
        printf("sim: flash_range_program(0x%06x, %zu) is not page aligned\n", off, n); return;
    }
    for (size_t i = 0; i < n; i++) sim_flash[off + i] &= data[i];   /* programming only clears bits, as on the chip */
}

/* ---- the watchdog and the boot ROM ---------------------------------------------------- */

static watchdog_hw_t s_wd;
watchdog_hw_t *watchdog_hw = &s_wd;

__attribute__((constructor)) static void scratch_restore(void)
{
    const char *s = getenv("SIM_SCRATCH0");             /* survives a reboot, like the real register */
    if (s) s_wd.scratch[0] = (uint32_t)strtoul(s, NULL, 0);
}

void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms)
{
    (void)pc; (void)sp; (void)delay_ms;
    char v[16];
    snprintf(v, sizeof v, "0x%08x", s_wd.scratch[0]);
    setenv("SIM_SCRATCH0", v, 1);
    sim_reboot("watchdog reboot");
}

void reset_usb_boot(uint32_t a, uint32_t b)
{
    (void)a; (void)b;
    sim_reboot("the USB bootloader was asked for (the panel would now wait for a new image)");
}
void rom_reset_usb_boot(uint32_t a, uint32_t b) { reset_usb_boot(a, b); }

/* The device's table (tools/pt.json): A at 8K and B after it, 5632K each. */
int rom_get_partition_table_info(uint32_t *out, uint32_t words, uint32_t flags)
{
    static const uint32_t first[2] = { 2, 2 + 1408 }, last[2] = { 2 + 1408 - 1, 2 + 2 * 1408 - 1 };
    if (flags & PT_INFO_PT_INFO) {
        if (words < 2) return -1;
        out[0] = flags; out[1] = 0x100 | 2;
        return 2;
    }
    if ((flags & PT_INFO_SINGLE_PARTITION) && (flags & PT_INFO_PARTITION_LOCATION_AND_FLAGS)) {
        uint32_t i = flags >> 24;
        if (i > 1 || words < 3) return -1;
        out[0] = PT_INFO_PARTITION_LOCATION_AND_FLAGS;
        out[1] = (first[i] << PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) | (last[i] << PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB);
        out[2] = 0;
        return 3;
    }
    return -1;
}

/* ---- over-the-air updates: the boot ROM's side (src/ota.c runs as on the device) ---- */
static qmi_hw_t s_qmi; static xip_ctrl_hw_t s_xipc;
qmi_hw_t *qmi_hw = &s_qmi;
xip_ctrl_hw_t *xip_ctrl_hw = &s_xipc;

int rom_get_uf2_target_partition(uint8_t *work, uint32_t n, uint32_t family, resident_partition_t *out)
{
    (void)work; (void)n; (void)family;
    uint32_t w[3];
    if (rom_get_partition_table_info(w, 3, PT_INFO_SINGLE_PARTITION | PT_INFO_PARTITION_LOCATION_AND_FLAGS | (1u << 24)) != 3) return -1;
    out->permissions_and_location = w[1]; out->permissions_and_flags = w[2];
    return 1;                                                    /* running from A: an update goes to B */
}
int  rom_explicit_buy(uint8_t *work, uint32_t n) { (void)work; (void)n; return 0; }
bool rom_get_boot_info(boot_info_t *out) { memset(out, 0, sizeof *out); out->partition = 0; return true; }
int  rom_get_last_boot_type(void) { return BOOT_TYPE_NORMAL; }
int  rom_reboot(uint32_t flags, uint32_t delay_ms, uint32_t p0, uint32_t p1)
{
    (void)flags; (void)delay_ms; (void)p1;
    printf("sim: the update is in slot B at 0x%06lx and checked out; a Mac can't start an RP2350 image, "
           "so the simulator restarts on this firmware (slot B is cleared at boot, as on the device)\n",
           (unsigned long)(p0 - (uint32_t)XIP_BASE));
    sim_reboot("over-the-air update");
}

/* ---- the board id: fixed, so Tesserae sees one simulator, not a new one each run ---- */

void pico_get_unique_board_id(pico_unique_board_id_t *id)
{
    static const uint8_t ID[8] = { 0x5E, 0x5E, 0x00, 0x00, 0xB3, 0xD5, 0x51, 0x3D };
    memcpy(id->id, ID, sizeof ID);
}
void pico_get_unique_board_id_string(char *out, uint len)
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    char b[17];
    for (int i = 0; i < 8; i++) snprintf(b + 2 * i, 3, "%02X", id.id[i]);
    snprintf(out, len, "%s", b);
}

uint32_t get_rand_32(void) { return arc4random(); }
uint64_t get_rand_64(void) { return ((uint64_t)arc4random() << 32) | arc4random(); }
