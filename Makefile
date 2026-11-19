DRIVER_ROOT ?= deps/rak2287-radio
RADIO_ROOT := $(abspath $(DRIVER_ROOT))
BUILD ?= $(CURDIR)/build
BUILD_ROOT := $(abspath $(BUILD))

RADIO_BUILD = $(MAKE) -C "$(RADIO_ROOT)/integration" -f Makefile \
	-f "$(CURDIR)/integration/radio-lab.mk" LAB_ROOT="$(CURDIR)" BUILD="$(BUILD_ROOT)" \
	DRIVER="$(RADIO_ROOT)/driver" HAL="$(RADIO_ROOT)/deps/hal" USP="$(RADIO_ROOT)/deps/usp" \
	MESHCORE="$(RADIO_ROOT)/deps/meshcore" CRYPTO="$(RADIO_ROOT)/deps/crypto"

.PHONY: all check hal
.NOTPARALLEL:

all: hal
	$(RADIO_BUILD) all radio-repeater lab-broker

check: hal
	$(MAKE) -C "$(RADIO_ROOT)/driver" HAL="$(RADIO_ROOT)/deps/hal" USP="$(RADIO_ROOT)/deps/usp" check
	$(RADIO_BUILD) lab-check

hal:
	@test -f "$(RADIO_ROOT)/driver/sx1250_dual.h" || \
		{ printf '%s\n' 'Initialize the pinned driver with git submodule update --init --recursive.' >&2; exit 1; }
	$(MAKE) -C "$(RADIO_ROOT)" hal
