#pragma once
#include "button_input.h"
#include "esp_err.h"

esp_err_t attention_provisioning_start(void);
// Called only by the button task, including when no navigation event occurred.
// Returns true while a modal consumes navigation/voice button input.
bool attention_provisioning_tick(button_input_event_t event);
bool attention_provisioning_active(void);
