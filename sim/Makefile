# The Bedside simulator: the firmware built for macOS. From the repo root:
#   make -C sim            build sim/build/bedside-sim
#   make -C sim run        build and start it (http://localhost:8080/sim)
# See sim/README.md.
CC      ?= cc
ROOT    := ..
OUT     := build
# make SAN=1: AddressSanitizer + UBSan, in build-san (out-of-bounds, use after
# free, overflow, misaligned access in the firmware's own code show as reports)
ifeq ($(SAN),1)
  OUT    := build-san
  SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
endif
PORT    ?= 8080

# macOS: prefer a 26.x SDK (as tests/Makefile: the 27 SDK's libSystem won't link host code here)
SYSROOT ?= $(firstword $(wildcard /Library/Developer/CommandLineTools/SDKs/MacOSX26*.sdk))
ifeq ($(SYSROOT),)
  SYSROOT := $(shell xcrun --show-sdk-path 2>/dev/null)
endif

VERSION := $(shell git -C $(ROOT) describe --tags --always 2>/dev/null | sed 's/^v//')-sim

# The firmware's own sources, unchanged. Left out (replaced by sim/src): the
# panel driver (it8951, epd_io), the key boards' I2C (seesaw), WiFi and the web
# server (net), audio output, the battery monitor, PSRAM bring-up, the
# paint test, and lwIP / DHCP / DNS server code.
FW_SRC := main commands paint screens painter menu inputs live config store psram_heap draw boot_logo \
          patterns alarm_sched birds rainsynth spk_dsp pcm_resample sha256 soma soma_snapshot http_chunked \
          icy tls_roots ota_version radio ha \
          ui_alarm ui_api ui_core ui_draw ui_live ui_overlay ui_pages ui_tree ui_wizard
FW_FILES := $(addprefix $(ROOT)/src/,$(addsuffix .c,$(FW_SRC))) \
            $(wildcard $(ROOT)/src/fonts/*.c) $(ROOT)/src/vendor/cJSON.c $(ROOT)/src/vendor/font24.c
SIM_FILES := $(wildcard src/*.c)

CFLAGS  ?= -O1 -g
CFLAGS  += $(SANFLAGS)
CFLAGS  += -std=gnu11 -isysroot $(SYSROOT) -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-missing-field-initializers -Wno-initializer-overrides -Wno-pointer-sign -Wno-unknown-pragmas \
           -Wno-format-truncation -Wno-unused-const-variable -Wno-sign-compare -Wno-macro-redefined
DEFS    := -DSIM=1 -DSTORE_HOST -DFW_VERSION=\"$(VERSION)\" \
           -DWIFI_SSID=\"Simulator\" -DWIFI_PASS=\"\" -DSIM_ROOT=\"$(abspath $(ROOT))\"
# The header mirror comes first: the firmware's headers, with sim/override in
# place of the ones that name hardware addresses (psram.h).
INC     := -I$(OUT)/inc -Iinclude -I$(ROOT)/src
# mbedTLS: the Pico SDK's copy, as the device builds it (scripts/build_mbedtls.py),
# with the firmware's config: https in the simulator is the device's TLS.
MBED    ?= $(HOME)/.platformio/packages/framework-picosdk/lib/mbedtls
ifeq ($(wildcard $(MBED)/include/mbedtls/ssl.h),)
  $(error mbedTLS not found at $(MBED): build the firmware once with PlatformIO, or pass MBED=<path to mbedtls>)
endif
MBED_DEF := -DMBEDTLS_CONFIG_FILE=\"bedside_mbedtls_config.h\"
INC     += -I$(MBED)/include
FW_CFLAGS := $(CFLAGS) $(DEFS) $(MBED_DEF) $(INC) -include prelude.h
LDLIBS  := -framework AudioToolbox -framework CoreFoundation -lm -lpthread

FW_OBJ  := $(patsubst $(ROOT)/src/%.c,$(OUT)/fw/%.o,$(FW_FILES))
SIM_OBJ := $(patsubst src/%.c,$(OUT)/sim/%.o,$(SIM_FILES)) $(OUT)/sim/assets.o
MBED_OBJ := $(patsubst $(MBED)/library/%.c,$(OUT)/mbedtls/%.o,$(wildcard $(MBED)/library/*.c))
BIN     := $(OUT)/bedside-sim

all: $(BIN)
fw: $(FW_OBJ)            # the firmware's files alone: a quick check that they still build for the host

$(OUT)/inc/.stamp: $(wildcard $(ROOT)/include/*.h) $(wildcard override/*.h)
	@mkdir -p $(OUT)/inc && rm -f $(OUT)/inc/*.h
	@for h in $(abspath $(ROOT))/include/*.h; do ln -sf $$h $(OUT)/inc/; done
	@for h in $(abspath override)/*.h; do ln -sf $$h $(OUT)/inc/; done
	@touch $@

# main.c's main() becomes firmware_main(): sim/src/sim_main.c starts the host side first
$(OUT)/fw/main.o: $(ROOT)/src/main.c $(OUT)/inc/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(FW_CFLAGS) -Dmain=firmware_main -MMD -c $< -o $@
# net_rest.c, tesserae.c, ui_menu.c and ota.c are compiled inside small wrappers (sim/src/wrap_*.c)
# that add the simulator's clock and time zone hooks next to their statics.
$(OUT)/fw/%.o: $(ROOT)/src/%.c $(OUT)/inc/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(FW_CFLAGS) -MMD -c $< -o $@
$(OUT)/sim/%.o: src/%.c $(OUT)/inc/.stamp $(OUT)/setup_page.inc
	@mkdir -p $(dir $@)
	$(CC) $(FW_CFLAGS) -Isrc -I$(OUT) -MMD -c $< -o $@
$(OUT)/sim/assets.o: src/assets.S $(ROOT)/assets/birds/birds.ulaw
	@mkdir -p $(dir $@)
	$(CC) -isysroot $(SYSROOT) -I$(ROOT) -c $< -o $@

# mbedTLS's own files: its warnings are its own
$(OUT)/mbedtls/%.o: $(MBED)/library/%.c $(ROOT)/include/bedside_mbedtls_config.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -w $(MBED_DEF) -I$(ROOT)/include -I$(MBED)/include -I$(MBED)/library -c $< -o $@

# The WiFi setup page lives in src/net.c as a C string; lift it out so the
# simulator serves the same page.
$(OUT)/setup_page.inc: $(ROOT)/src/net.c
	@mkdir -p $(OUT)
	@awk '/^static const char SETUP_PAGE\[\] =/{on=1; sub(/^static const char SETUP_PAGE\[\] =/, "")} on{print} on && /";$$/{exit}' $< > $@

# The web app pages, as PlatformIO's pre-build script makes them
www:
	@python3 tools/build_www_host.py

$(BIN): www $(FW_OBJ) $(SIM_OBJ) $(MBED_OBJ)
	$(CC) $(CFLAGS) $(FW_OBJ) $(SIM_OBJ) $(MBED_OBJ) $(LDLIBS) -o $@
	@echo "built $@ ($(VERSION))"

run: $(BIN)
	./$(BIN) --port $(PORT)

clean:                   # keeps build/flash.bin: the simulated panel's settings
	rm -rf $(OUT)/fw $(OUT)/sim $(OUT)/mbedtls $(OUT)/inc $(OUT)/setup_page.inc $(BIN)

-include $(FW_OBJ:.o=.d) $(SIM_OBJ:.o=.d)
.PHONY: all fw run clean www
