BOARD ?= xiao_ble/nrf52840/sense
BUILD_DIR ?= $(CURDIR)/../../build/$(subst /,-,$(BOARD))
NRFUTIL ?= adafruit-nrfutil
APP_HEX := $(BUILD_DIR)/nordic-nrf52840/zephyr/zephyr.hex
DFU_ZIP := $(BUILD_DIR)/anchor-dfu.zip
CONTROL_CONF := $(CURDIR)/conf/control.conf
ifeq ($(BOARD),xiao_ble/nrf52840/sense)
BOARD_EXTRA_ARGS := -DDTC_OVERLAY_FILE=$(CURDIR)/boards/xiao_ble_nrf52840_sense.overlay
endif

.PHONY: build dfu-package test clean

build:
	west build -p always -b $(BOARD) $(CURDIR) -d $(BUILD_DIR) -- -DEXTRA_CONF_FILE=$(CONTROL_CONF) $(BOARD_EXTRA_ARGS)

dfu-package:
	$(NRFUTIL) dfu genpkg --dev-type 0x0052 --dev-revision 52840 --application "$(APP_HEX)" "$(DFU_ZIP)"
	python3 tests/check_dfu_package.py "$(DFU_ZIP)"

test:
	mkdir -p $(CURDIR)/build-host
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror tests/trainer_test.c -o $(CURDIR)/build-host/trainer-test
	$(CURDIR)/build-host/trainer-test

clean:
	rm -rf $(CURDIR)/build-host $(BUILD_DIR)
