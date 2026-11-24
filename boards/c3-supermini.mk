# 4MB flash; two 0x1F0000 (1.9375MiB) OTA slots, no filesystem.
BOARD_FQBN := esp32:esp32:esp32c3:PartitionScheme=no_fs,CDCOnBoot=cdc
BOARD_NAME := ESP32-C3 Super Mini
BOARD_HEADER_DIR := boards/c3-supermini
BOARD_IMAGE_PREFIX := ANCHOR-C3-SUPERMINI
BOARD_FULL_SIZE := 4194304
BOARD_OTA_SLOT := 0x1F0000
BOARD_CHIP_FAMILY := ESP32-C3
BOARD_BOOT_OFFSET := 0x0
BOARD_INSTALL_STATUS := Experimental; compile tested only
