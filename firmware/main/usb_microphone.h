#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t usb_microphone_init(void);
bool usb_microphone_host_active(void);
