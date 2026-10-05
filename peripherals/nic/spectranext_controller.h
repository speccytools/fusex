#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "libspectrum.h"
#include "memory_pages.h"
#include "spectranext.h"

#define SPECTRANEXT_CONTROLLER_XFS_READ_BUFFER_OFFSET 0x900u

extern volatile struct spectranext_controller_t spectranext_controller;

extern void spectranext_controller_init(void);

bool spectranext_controller_post_message(const char *message);
bool spectranext_controller_post_message_bytes(const uint8_t *message, size_t length);
void spectranext_controller_clear_messages(void);

libspectrum_byte spectranext_controller_read(memory_page *page, libspectrum_word address);
void spectranext_controller_write(memory_page *page, libspectrum_word address, libspectrum_byte b);
