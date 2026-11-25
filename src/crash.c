/*
 * crash.c: a system error restarts the panel, and leaves word of what it was
 *
 * A hard fault (a bad pointer, an undefined instruction, a fault that could
 * not be handled) or an SDK panic() used to stop the panel where it stood,
 * with the last frame on the glass and a clock that no longer ran. Now it
 * writes what happened into the watchdog's scratch registers, which survive
 * the restart, and restarts; main.c reads them at boot and the UI says so
 * (the System 7 theme with its bomb, local_system_error()).
 *
 * A fault that comes back within a minute of every start would restart for
 * ever, the glass flashing each time: after CRASH_EARLY_MAX of those in a row
 * the panel stops where it is, as it used to. main.c clears the count once a
 * start has run for a minute.
 *
 * The device only: the simulator has no faults to take (its "crash=test"
 * command exercises everything after the restart).
 */
#include "app_internal.h"
#include <stdarg.h>

#define CRASH_EARLY_MAX 3

static void __attribute__((noreturn)) crash_restart(uint32_t kind, uint32_t pc)
{
    if (time_us_32() < 60u * 1000000u && ++watchdog_hw->scratch[3] > CRASH_EARLY_MAX)
        for (;;) __asm volatile("bkpt #0");          /* it keeps happening at once: stop here, for a debugger */
    crash_record(kind, pc);
    watchdog_reboot(0, 0, 0);
    for (;;) { }
}

/* The frame the core stacked when it took the fault: r0-r3, r12, lr, pc, xPSR. */
static void __attribute__((used, noreturn)) fault_c(const uint32_t *frame)
{
    crash_restart(CRASH_FAULT, frame[6]);
}

/* Replaces the SDK's weak handler (a breakpoint). Which stack the frame is on is in lr. */
void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b fault_c\n");
}

/* The SDK's panic() comes here (PICO_PANIC_FUNCTION in platformio.ini): say why, then as a fault. */
void __attribute__((noreturn)) crash_panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("\n*** panic: "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    crash_restart(CRASH_PANIC, (uint32_t)(uintptr_t)__builtin_return_address(0));
}
