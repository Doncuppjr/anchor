# Build orchestrator only; Arduino CLI owns the ESP32 toolchain.
# make BOARD=c5 PROFILE=production
# make BOARD=c3-supermini PROFILE=debug
# Shortcut: make c5 PROD=TRUE or make c3-supermini PROD=FALSE

ARDUINO_CLI ?= arduino-cli
BOARD ?= c5
PROFILE ?= production
ifneq ($(filter TRUE FALSE,$(PROD)),)
  ifeq ($(PROD),TRUE)
    PROFILE := production
  else
    PROFILE := debug
  endif
endif
ifeq ($(filter $(PROFILE),production debug),)
$(error PROFILE must be production or debug)
endif
ifeq ($(PROFILE),production)
  DEBUG_FLAG := 0
else
  DEBUG_FLAG := 1
endif
BOARD_FILE := boards/$(BOARD).mk
ifeq ($(wildcard $(BOARD_FILE)),)
$(error Unknown BOARD=$(BOARD); expected $(BOARD_FILE))
endif
include $(BOARD_FILE)
SKETCH := firmware/anchor
OUT := dist/$(BOARD)/$(PROFILE)
APP := $(OUT)/$(BOARD_IMAGE_PREFIX)-$(PROFILE)-app.bin
FULL := $(OUT)/$(BOARD_IMAGE_PREFIX)-$(PROFILE)-full.bin

.PHONY: all build c5 c3-supermini test clean help
.DEFAULT_GOAL := build
all: build
c5:
	@$(MAKE) --no-print-directory BOARD=c5 build
c3-supermini:
	@$(MAKE) --no-print-directory BOARD=c3-supermini build
build:
	@mkdir -p '$(OUT)'
	$(ARDUINO_CLI) compile --fqbn '$(BOARD_FQBN)' \
	  --build-property 'compiler.cpp.extra_flags=-DANCHOR_DEBUG=$(DEBUG_FLAG) -I$(abspath $(BOARD_HEADER_DIR))' $(BOARD_EXTRA_BUILD_PROPERTIES) \
	  --output-dir '$(OUT)' '$(SKETCH)'
	cp '$(OUT)/anchor.ino.bin' '$(APP)'
	cp '$(OUT)/anchor.ino.merged.bin' '$(FULL)'
	@test "$$(wc -c < '$(FULL)')" -eq '$(BOARD_FULL_SIZE)'
	@python3 -c 'from pathlib import Path; p=Path("$(APP)"); assert p.stat().st_size <= int("$(BOARD_OTA_SLOT)",0), f"OTA slot overflow: {p.stat().st_size} bytes"'
	cd '$(OUT)' && sha256sum '$(notdir $(APP))' '$(notdir $(FULL))' > SHA256SUMS
	@cat '$(OUT)/SHA256SUMS'
test:
	@mkdir -p build
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/test_trainer_protocol.cpp -o build/test_trainer_protocol
	./build/test_trainer_protocol
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/test_uart_maintenance.cpp -o build/test_uart_maintenance
	./build/test_uart_maintenance
clean:
	rm -rf build dist
help:
	@printf 'make BOARD=c5 PROFILE=production\nmake BOARD=c3-supermini PROFILE=debug\nmake c5 PROD=TRUE\nmake c3-supermini PROD=FALSE\nmake test | make clean\n'
