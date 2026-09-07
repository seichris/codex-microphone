#include "attention_connection.h"
const char *attention_connection_error(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return "Connected";
    case ATTENTION_ERR_UNPAIRED: return "Pair with your Mac over serial";
    case ATTENTION_ERR_STORAGE: return "Secure pairing storage unavailable";
    case ATTENTION_ERR_WIFI: return "Wi-Fi disconnected";
    case ATTENTION_ERR_DISCOVERY: return "Paired bridge not discovered";
    case ATTENTION_ERR_TLS: return "Paired bridge TLS verification failed";
    case ATTENTION_ERR_UNAUTHORIZED: return "Pairing revoked or authentication failed";
    case ATTENTION_ERR_RESET_PENDING: return "Reset pending bridge revocation";
    case ATTENTION_ERR_UNAVAILABLE: return "Paired bridge unavailable";
    default: return "Invalid bridge response";
    }
}
