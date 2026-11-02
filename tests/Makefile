# Host tests: plain C, no Pico SDK. From the repo root:  make -C tests
# (CI runs the same on every push; see .github/workflows/build.yml.)
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -Wno-override-init
ROOT    := ..
INC     := -I$(ROOT)/include -I. -Ihost
# macOS: the Command Line Tools need an SDK path. The MacOSX27 SDK's libSystem
# won't link host code here, so prefer a 26.x SDK when one is installed.
SYSROOT ?= $(firstword $(wildcard /Library/Developer/CommandLineTools/SDKs/MacOSX26*.sdk))
ifeq ($(SYSROOT),)
  SYSROOT := $(shell xcrun --show-sdk-path 2>/dev/null)
endif
ifneq ($(SYSROOT),)
  CFLAGS += -isysroot $(SYSROOT)
endif
OUT     := build

TESTS := alarm store misc sound

# Run every suite, even after a failure, then fail if any did.
all: $(addprefix $(OUT)/test_,$(TESTS))
	@fail=0; for t in $(TESTS); do ./$(OUT)/test_$$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "all host tests passed"; else echo "HOST TESTS FAILED"; exit 1; fi

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/test_alarm: test_alarm.c $(ROOT)/src/alarm_sched.c | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -lm -o $@
$(OUT)/test_store: test_store.c host/store_host.c $(ROOT)/src/store.c | $(OUT)
	$(CC) $(CFLAGS) $(INC) -DSTORE_HOST $^ -lm -o $@
$(OUT)/test_misc: test_misc.c $(ROOT)/src/sha256.c $(ROOT)/src/ota_version.c $(ROOT)/src/http_chunked.c | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -lm -o $@
$(OUT)/test_sound: test_sound.c $(ROOT)/src/rainsynth.c $(ROOT)/src/birds.c $(ROOT)/src/spk_dsp.c | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -lm -o $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
