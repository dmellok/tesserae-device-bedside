/*
 * store_host.h for the simulator: src/store.c built with -DSTORE_HOST reads
 * the mapped flash file and asks the simulated boot ROM for the partitions.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "sim_sdk.h"

#define FLASH_RAW ((const uint8_t *)sim_flash)
#define STORE_IMAGE_END() (2u * 1024 * 1024)     /* the firmware image, as on the device */
void     config_flash_sector(uint32_t off, const uint8_t *data, size_t len);
uint8_t *config_sector_buf(void);
