# Seeed D7 RX / D6 TX / D1 enable; 4MB minimal SPIFFS dual OTA
BOARD_FQBN := esp32:esp32:XIAO_ESP32C3:PartitionScheme=min_spiffs,CDCOnBoot=cdc
BOARD_NAME := XIAO ESP32-C3
BOARD_HEADER_DIR := boards/xiao-c3
BOARD_IMAGE_PREFIX := ANCHOR-XIAO-C3
BOARD_FULL_SIZE := 4194304
BOARD_OTA_SLOT := 0x1E0000
BOARD_CHIP_FAMILY := ESP32-C3
BOARD_BOOT_OFFSET := 0x0
BOARD_INSTALL_STATUS := Experimental; compilation and hardware tests pending
