/*
 * sim_main.c: the simulator's entry point. Sets up the host side (options, the
 * flash file, the terminal), then runs the firmware's own main() (src/main.c,
 * built as firmware_main). A reboot runs this process again.
 */
#include "sim.h"
#include "sim_sdk.h"

#include <mach-o/dyld.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

int firmware_main(void);

static char **s_argv;
static struct termios s_tty;
static bool s_tty_raw;

static void tty_restore(void) { if (s_tty_raw) tcsetattr(0, TCSANOW, &s_tty); s_tty_raw = false; }
static void on_signal(int sig) { tty_restore(); sim_flash_sync(); signal(sig, SIG_DFL); raise(sig); }

/* Keys reach the firmware's serial console one at a time (m menu, j/k turn,
 * Enter press, ...; "?" lists them), as over USB. */
static void tty_raw(void)
{
    if (!isatty(0) || tcgetattr(0, &s_tty) != 0) return;
    struct termios t = s_tty;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) == 0) s_tty_raw = true;
}

void sim_reboot(const char *why)
{
    printf("\nsim: %s: restarting\n", why);
    fflush(stdout);
    sim_flash_sync();
    tty_restore();
    char w[24];
    snprintf(w, sizeof w, "%lld", (long long)sim_warp_total_s());
    setenv("SIM_WARP_S", w, 1);                       /* the wall clock keeps its warp */
    char self[1024];
    uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) != 0) snprintf(self, sizeof self, "%s", s_argv[0]);
    execv(self, s_argv);
    perror("sim: restart failed");
    exit(1);
}

static void usage(const char *me)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --port N        web server port (default 8080): the phone app at /, the simulator at /sim\n"
        "  --flash FILE    the flash image (default sim/build/flash.bin): settings, pairing, files\n"
        "  --reset         start from an erased flash (a new panel: WiFi seeded, first-run setup)\n"
        "  --instant       refreshes take no time (default: the panel's real timings)\n"
        "  --no-audio      no sound\n"
        "  --no-knob, --no-keys   as if that board were unplugged\n", me);
}

int main(int argc, char **argv)
{
    s_argv = argv;
    static char flash_default[1024];
    snprintf(flash_default, sizeof flash_default, "%s/sim/build/flash.bin", SIM_ROOT);
    sim_opt.flash_path = flash_default;
    bool reset = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--port") && i + 1 < argc)       sim_opt.port = atoi(argv[++i]);
        else if (!strcmp(a, "--flash") && i + 1 < argc) sim_opt.flash_path = argv[++i];
        else if (!strcmp(a, "--reset"))                 reset = true;
        else if (!strcmp(a, "--instant"))               sim_opt.instant = true;
        else if (!strcmp(a, "--no-audio"))              sim_opt.no_audio = true;
        else if (!strcmp(a, "--no-knob"))               sim_opt.no_knob = true;
        else if (!strcmp(a, "--no-keys"))               sim_opt.no_keys = true;
        else { usage(argv[0]); return a[2] == 'h' ? 0 : 2; }
    }
    if (reset && !getenv("SIM_STARTED")) unlink(sim_opt.flash_path);   /* only the first start, not a reboot */
    setenv("SIM_STARTED", "1", 1);
    sim_flash_open(sim_opt.flash_path);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);              /* a browser closing a connection mid-reply */
    atexit(tty_restore);
    tty_raw();

    fprintf(stderr, "sim: flash %s; open http://localhost:%d/sim\n", sim_opt.flash_path, sim_opt.port);
    return firmware_main();
}
