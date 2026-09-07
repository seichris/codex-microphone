#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_TIMEOUT -4
#define ESP_ERR_NO_MEM -5
#define ESP_ERR_NOT_FOUND -6
#define ESP_CODEC_DEV_OK 0
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY UINT32_MAX
struct fake_semaphore { int available; bool mutex; };
typedef struct fake_semaphore *SemaphoreHandle_t;
struct fake_queue;
typedef struct fake_queue *QueueHandle_t;
typedef void *TaskHandle_t;
typedef void *esp_codec_dev_handle_t;
typedef struct {
    int clk_cfg, slot_cfg;
    struct { int mclk, bclk, ws, dout, din;
        struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags;
    } gpio_cfg;
} i2s_std_config_t;
typedef struct { int bits_per_sample, channel, channel_mask, sample_rate, mclk_multiple; } esp_codec_dev_sample_info_t;
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(bits, mode) (bits)
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
#define BSP_I2S_MCLK 1
#define BSP_I2S_SCLK 2
#define BSP_I2S_LCLK 3
#define BSP_I2S_DOUT 4
#define BSP_I2S_DSIN 5
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
QueueHandle_t xQueueCreate(unsigned, size_t);
int xQueueReset(QueueHandle_t);
int xQueueSend(QueueHandle_t, const void *, TickType_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
esp_err_t bsp_audio_init(const i2s_std_config_t *);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
int esp_codec_dev_open(esp_codec_dev_handle_t, const esp_codec_dev_sample_info_t *);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t, float);
int esp_codec_dev_read(esp_codec_dev_handle_t, void *, int);
