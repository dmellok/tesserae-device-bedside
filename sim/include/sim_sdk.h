/*
 * sim_sdk.h: the parts of the Pico SDK the firmware uses, for the simulator.
 *
 * Every pico/... and hardware/... header the firmware includes is a one-line
 * file in sim/include that includes this one. Time, sleeping, flash, the two
 * cores and the watchdog are real enough for the application to run unchanged;
 * the pins and peripherals it only touches through drivers (SPI, I2C, PIO,
 * DMA, ADC) are not here, because those drivers are replaced (sim/src).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef unsigned int uint;

/* ---- time: microseconds since boot, plus the simulator's time warp ---------- */
typedef uint64_t absolute_time_t;
uint64_t time_us_64(void);
static inline uint32_t time_us_32(void)                         { return (uint32_t)time_us_64(); }
static inline absolute_time_t get_absolute_time(void)           { return time_us_64(); }
static inline uint32_t to_ms_since_boot(absolute_time_t t)       { return (uint32_t)(t / 1000u); }
static inline uint64_t to_us_since_boot(absolute_time_t t)       { return t; }
static inline absolute_time_t make_timeout_time_ms(uint32_t ms)  { return time_us_64() + (uint64_t)ms * 1000u; }
static inline absolute_time_t make_timeout_time_us(uint64_t us)  { return time_us_64() + us; }
static inline absolute_time_t delayed_by_ms(absolute_time_t t, uint32_t ms) { return t + (uint64_t)ms * 1000u; }
static inline absolute_time_t delayed_by_us(absolute_time_t t, uint64_t us) { return t + us; }
static inline bool time_reached(absolute_time_t t)               { return time_us_64() >= t; }
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) { return (int64_t)(to - from); }
static inline bool is_nil_time(absolute_time_t t)                { return t == 0; }
#define nil_time ((absolute_time_t)0)
void sleep_ms(uint32_t ms);
void sleep_us(uint64_t us);
static inline void busy_wait_us(uint64_t us)    { sleep_us(us); }
static inline void busy_wait_us_32(uint32_t us) { sleep_us(us); }
static inline void busy_wait_ms(uint32_t ms)    { sleep_ms(ms); }
static inline void tight_loop_contents(void)    {}

typedef int32_t alarm_id_t;
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void *user_data);
alarm_id_t add_alarm_in_ms(uint32_t ms, alarm_callback_t cb, void *user_data, bool fire_if_past);
bool cancel_alarm(alarm_id_t id);

#define PICO_OK               0
#define PICO_ERROR_NONE       0
#define PICO_ERROR_TIMEOUT   -1
#define PICO_ERROR_GENERIC   -2
#define PICO_ERROR_NO_DATA   -3

/* ---- stdio: printf goes to the terminal and to the firmware's log driver ---- */
typedef struct stdio_driver {
    void (*out_chars)(const char *buf, int len);
    void (*out_flush)(void);
    int  (*in_chars)(char *buf, int len);
    void (*set_chars_available_callback)(void (*fn)(void *), void *param);
    struct stdio_driver *next;
    bool last_ended_with_cr;
    bool crlf_enabled;
} stdio_driver_t;
bool stdio_init_all(void);
void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled);
void stdio_flush(void);
int  getchar_timeout_us(uint32_t timeout_us);
int  sim_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  sim_putchar(int c);
int  sim_puts(const char *s);

/* ---- GPIO: inputs read high (nothing pressed), outputs go nowhere ----------- */
enum { GPIO_IN = 0, GPIO_OUT = 1 };
enum { GPIO_FUNC_SPI = 1, GPIO_FUNC_UART = 2, GPIO_FUNC_I2C = 3, GPIO_FUNC_PWM = 4, GPIO_FUNC_SIO = 5,
       GPIO_FUNC_PIO0 = 6, GPIO_FUNC_PIO1 = 7, GPIO_FUNC_NULL = 0x1f };
enum { GPIO_OVERRIDE_NORMAL = 0, GPIO_OVERRIDE_INVERT = 1, GPIO_OVERRIDE_LOW = 2, GPIO_OVERRIDE_HIGH = 3 };
enum { GPIO_SLEW_RATE_SLOW = 0, GPIO_SLEW_RATE_FAST = 1 };
enum { GPIO_DRIVE_STRENGTH_2MA, GPIO_DRIVE_STRENGTH_4MA, GPIO_DRIVE_STRENGTH_8MA, GPIO_DRIVE_STRENGTH_12MA };
static inline void gpio_init(uint pin)                    { (void)pin; }
static inline void gpio_deinit(uint pin)                  { (void)pin; }
static inline void gpio_set_dir(uint pin, bool out)       { (void)pin; (void)out; }
static inline void gpio_pull_up(uint pin)                 { (void)pin; }
static inline void gpio_pull_down(uint pin)               { (void)pin; }
static inline void gpio_disable_pulls(uint pin)           { (void)pin; }
static inline void gpio_put(uint pin, bool v)             { (void)pin; (void)v; }
static inline void gpio_set_function(uint pin, int fn)    { (void)pin; (void)fn; }
static inline void gpio_set_input_enabled(uint pin, bool e) { (void)pin; (void)e; }
bool sim_gpio_get(uint pin);
static inline bool gpio_get(uint pin)                     { return sim_gpio_get(pin); }

/* main.c's BOOTSEL reader (unused on the host, but compiled) */
typedef struct { struct { volatile uint32_t status, ctrl; } io[6]; } ioqspi_hw_t;
typedef struct { volatile uint32_t gpio_in, gpio_hi_in; } sio_hw_t;
extern ioqspi_hw_t *ioqspi_hw;
extern sio_hw_t *sio_hw;
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB  12
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS 0x00003000u
#define SIO_GPIO_HI_IN_QSPI_CSN_BITS          0x04000000u
static inline void hw_write_masked(volatile uint32_t *a, uint32_t v, uint32_t m) { *a = (*a & ~m) | (v & m); }
#define __no_inline_not_in_flash_func(f) f
#define __not_in_flash_func(f) f
#define __time_critical_func(f) f
#define __not_in_flash(g)
#define __in_flash(g)
#define __scratch_x(g)
#define __scratch_y(g)
#define __uninitialized_ram(v) v

/* ---- interrupts and barriers: no-ops; the two cores are threads ------------- */
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t s)       { (void)s; }
static inline void restore_interrupts_from_disabled(uint32_t s) { (void)s; }
#define __dmb()  __sync_synchronize()
#define __dsb()  __sync_synchronize()
#define __isb()  __sync_synchronize()
#define __compiler_memory_barrier() __asm__ volatile("" ::: "memory")
void sim_wfe(void);
#define __wfe()  sim_wfe()
#define __wfi()  sim_wfe()
#define __sev()  ((void)0)
#define __nop()  ((void)0)

/* ---- the second core: a thread --------------------------------------------- */
void multicore_launch_core1(void (*entry)(void));
void multicore_launch_core1_with_stack(void (*entry)(void), uint32_t *stack_bottom, size_t stack_size_bytes);
void multicore_reset_core1(void);
static inline void multicore_lockout_victim_init(void) {}
static inline bool multicore_lockout_victim_is_initialized(uint core) { (void)core; return false; }
static inline void multicore_lockout_start_blocking(void) {}
static inline void multicore_lockout_end_blocking(void) {}
static inline uint get_core_num(void) { return 0; }

typedef struct { pthread_mutex_t m; } mutex_t;
static inline void mutex_init(mutex_t *mx)            { pthread_mutex_init(&mx->m, NULL); }
static inline void mutex_enter_blocking(mutex_t *mx)  { pthread_mutex_lock(&mx->m); }
static inline bool mutex_try_enter(mutex_t *mx, uint32_t *owner) { (void)owner; return pthread_mutex_trylock(&mx->m) == 0; }
static inline void mutex_exit(mutex_t *mx)            { pthread_mutex_unlock(&mx->m); }
typedef mutex_t recursive_mutex_t;
typedef struct { pthread_mutex_t m; } critical_section_t;
static inline void critical_section_init(critical_section_t *c)    { pthread_mutex_init(&c->m, NULL); }
static inline void critical_section_enter_blocking(critical_section_t *c) { pthread_mutex_lock(&c->m); }
static inline void critical_section_exit(critical_section_t *c)    { pthread_mutex_unlock(&c->m); }
typedef struct { volatile int v; } spin_lock_t;
static inline spin_lock_t *spin_lock_instance(uint n) { static spin_lock_t l[32]; return &l[n & 31]; }
static inline uint spin_lock_claim_unused(bool required) { (void)required; return 0; }
static inline uint32_t spin_lock_blocking(spin_lock_t *l) { (void)l; return 0; }
static inline void spin_unlock(spin_lock_t *l, uint32_t s) { (void)l; (void)s; }

/* ---- flash: a 16 MB file mapped into memory (sim_flash.c) ------------------- */
extern uint8_t *sim_flash;
#define SIM_FLASH_BYTES (16u * 1024 * 1024)
#define XIP_BASE                            ((uintptr_t)sim_flash)
#define XIP_NOCACHE_NOALLOC_BASE            ((uintptr_t)sim_flash)
#define XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE ((uintptr_t)sim_flash)
#define FLASH_PAGE_SIZE   256u
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_BLOCK_SIZE  65536u
#define PICO_FLASH_SIZE_BYTES SIM_FLASH_BYTES
void flash_range_erase(uint32_t off, size_t count);
void flash_range_program(uint32_t off, const uint8_t *data, size_t count);

/* ---- the watchdog: a reboot starts the simulator again --------------------- */
typedef struct { volatile uint32_t ctrl, load, reason, scratch[8], tick; } watchdog_hw_t;
extern watchdog_hw_t *watchdog_hw;
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);
static inline void watchdog_enable(uint32_t ms, bool pause) { (void)ms; (void)pause; }
static inline void watchdog_update(void) {}
static inline bool watchdog_caused_reboot(void) { return false; }
static inline bool watchdog_enable_caused_reboot(void) { return false; }

/* ---- the boot ROM ---------------------------------------------------------- */
void reset_usb_boot(uint32_t gpio_mask, uint32_t disable_interface_mask);
void rom_reset_usb_boot(uint32_t a, uint32_t b);
int  rom_get_partition_table_info(uint32_t *out, uint32_t words, uint32_t flags);
#define PT_INFO_PT_INFO                       0x0001
#define PT_INFO_PARTITION_LOCATION_AND_FLAGS  0x0010
#define PT_INFO_SINGLE_PARTITION              0x8000
#define PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB  0
#define PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS 0x00001fffu
#define PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB   13
#define PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS  0x03ffe000u
/* over-the-air updates (src/ota.c): the simulator runs from slot A; an update
 * goes to B; a "reboot into the new image" restarts the simulator on the same
 * firmware (a Mac can't run an RP2350 image), which then clears slot B as the
 * device does when it finds an image it didn't boot. */
typedef struct { uint32_t permissions_and_location, permissions_and_flags; } resident_partition_t;
typedef struct { int8_t partition; uint8_t tbyb_and_update_info; int8_t recent_boot_partition, recent_boot_type; } boot_info_t;
#define RP2350_ARM_S_FAMILY_ID                 0xe48bff59u
#define BOOT_TYPE_NORMAL                       0
#define BOOT_TYPE_FLASH_UPDATE                 4
#define REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE  0x4
int  rom_get_uf2_target_partition(uint8_t *work, uint32_t work_size, uint32_t family, resident_partition_t *out);
int  rom_explicit_buy(uint8_t *work, uint32_t work_size);
bool rom_get_boot_info(boot_info_t *out);
int  rom_get_last_boot_type(void);
int  rom_reboot(uint32_t flags, uint32_t delay_ms, uint32_t p0, uint32_t p1);
typedef struct { volatile uint32_t timing, rfmt, rcmd, wfmt, wcmd, pad[3]; } qmi_mem_hw_t;
typedef struct { qmi_mem_hw_t m[2]; } qmi_hw_t;
typedef struct { volatile uint32_t ctrl; } xip_ctrl_hw_t;
extern qmi_hw_t *qmi_hw;
extern xip_ctrl_hw_t *xip_ctrl_hw;

/* ---- the board's unique id: fixed, so the simulator is one device ---------- */
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
typedef struct { uint8_t id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES]; } pico_unique_board_id_t;
void pico_get_unique_board_id(pico_unique_board_id_t *id);
void pico_get_unique_board_id_string(char *out, uint len);

/* ---- random numbers -------------------------------------------------------- */
uint32_t get_rand_32(void);
uint64_t get_rand_64(void);

/* ---- TinyUSB: main.c's 1200-baud reset hook --------------------------------- */
typedef struct { uint32_t bit_rate; uint8_t stop_bits, parity, data_bits; } cdc_line_coding_t;
