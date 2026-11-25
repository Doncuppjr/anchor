# Seeed D7 RX / D6 TX / D1 enable; GPIO3 reserved RF switch
BOARD_FQBN := esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc
BOARD_NAME := XIAO ESP32-C6
BOARD_HEADER_DIR := boards/xiao-c6
BOARD_IMAGE_PREFIX := ANCHOR-XIAO-C6
BOARD_FULL_SIZE := 4194304
BOARD_OTA_SLOT := 0x1F0000
BOARD_CHIP_FAMILY := ESP32-C6
BOARD_BOOT_OFFSET := 0x0
BOARD_INSTALL_STATUS := Experimental; compilation and hardware tests pending
BOARD_EXTRA_BUILD_PROPERTIES := --build-property build.partitions=no_fs --build-property upload.maximum_size=2031616
