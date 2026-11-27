BOARD ?= promicro_nrf52840/nrf52840/uf2
BUILD_DIR ?= $(CURDIR)/../../build/nordic-nrf52840
CONTROL_CONF := $(CURDIR)/conf/control.conf

.PHONY: build test clean

build:
	west build -p always -b $(BOARD) $(CURDIR) -d $(BUILD_DIR) -- -DEXTRA_CONF_FILE=$(CONTROL_CONF)

test:
	mkdir -p $(CURDIR)/build-host
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror tests/trainer_test.c -o $(CURDIR)/build-host/trainer-test
	$(CURDIR)/build-host/trainer-test

clean:
	rm -rf $(CURDIR)/build-host $(BUILD_DIR)
