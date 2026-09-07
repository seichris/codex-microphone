#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

#define ATTENTION_ID_MAX 49
#define ATTENTION_TITLE_MAX 97
#define ATTENTION_PROJECT_MAX 49
#define ATTENTION_KIND_MAX 17
#define ATTENTION_ERROR_MAX 161
#define ATTENTION_DETAIL_TEXT_MAX 6145

typedef enum {
    ATTENTION_STATUS_IDLE = 0,
    ATTENTION_STATUS_RUNNING,
    ATTENTION_STATUS_WAITING_INPUT,
    ATTENTION_STATUS_WAITING_APPROVAL,
    ATTENTION_STATUS_ERROR,
} attention_status_t;

typedef enum {
    ATTENTION_FOCUS_UNAVAILABLE = 0,
    ATTENTION_FOCUS_INFERRED,
    ATTENTION_FOCUS_CONFIRMED,
} attention_focus_confidence_t;

typedef enum {
    ATTENTION_VOICE_UNKNOWN = 0,
    ATTENTION_VOICE_READY,
    ATTENTION_VOICE_FOCUSING,
    ATTENTION_VOICE_STARTING,
    ATTENTION_VOICE_LISTENING,
    ATTENTION_VOICE_MUTED,
    ATTENTION_VOICE_ERROR,
} attention_voice_state_t;

typedef struct {
    char id[ATTENTION_ID_MAX];
    char title[ATTENTION_TITLE_MAX];
    char project[ATTENTION_PROJECT_MAX];
    attention_status_t status;
    uint32_t age_seconds;
    bool unread;
    bool pinned;
    bool new_result;
} attention_item_t;

typedef struct {
    bool available;
    char id[ATTENTION_ID_MAX];
    char title[ATTENTION_TITLE_MAX];
    char project[ATTENTION_PROJECT_MAX];
    attention_status_t status;
    attention_focus_confidence_t focus_confidence;
    attention_voice_state_t voice_state;
} attention_current_thread_t;

typedef struct {
    bool desktop_focus;
    bool desktop_voice_hotkey;
    bool power_button_long_press;
    bool wireless_microphone;
} attention_capabilities_t;

typedef enum {
    ATTENTION_DESKTOP_CONTROL_UNKNOWN = 0,
    ATTENTION_DESKTOP_CONTROL_UNAVAILABLE,
    ATTENTION_DESKTOP_CONTROL_AVAILABLE,
} attention_desktop_control_availability_t;

typedef struct {
    uint32_t count;
    uint32_t total_count;
    bool truncated;
    bool desktop_state_available;
    attention_desktop_control_availability_t desktop_control_availability;
    attention_current_thread_t current_thread;
    attention_capabilities_t capabilities;
    char source_error[ATTENTION_ERROR_MAX];
    attention_item_t items[CONFIG_CODEX_ATTENTION_MAX_ITEMS];
} attention_snapshot_t;

typedef struct {
    char request_id[97];
    char thread_id[ATTENTION_ID_MAX];
    attention_focus_confidence_t focus_confidence;
    attention_voice_state_t voice_state;
    attention_capabilities_t capabilities;
} attention_desktop_state_t;

typedef struct {
    char id[ATTENTION_ID_MAX];
    char title[ATTENTION_TITLE_MAX];
    char project[ATTENTION_PROJECT_MAX];
    char kind[ATTENTION_KIND_MAX];
    char text[ATTENTION_DETAIL_TEXT_MAX];
    char source_error[ATTENTION_ERROR_MAX];
    bool truncated;
} attention_detail_t;
