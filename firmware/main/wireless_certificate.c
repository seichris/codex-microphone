#include "wireless_certificate.h"
#include <string.h>

bool wireless_certificate_decode(const char *input, char *output, size_t capacity)
{
    if (input == NULL || output == NULL || capacity == 0) return false;
    size_t used = 0;
    while (*input != '\0') {
        char value = *input++;
        if (value == '\\') {
            if (*input == 'n') value = '\n';
            else if (*input == 'r') value = '\r';
            else return false;
            ++input;
        }
        if (used + 1 >= capacity) return false;
        output[used++] = value;
    }
    output[used] = '\0';
    return strncmp(output, "-----BEGIN CERTIFICATE-----\n", sizeof("-----BEGIN CERTIFICATE-----\n") - 1) == 0
        || strncmp(output, "-----BEGIN CERTIFICATE-----\r\n", sizeof("-----BEGIN CERTIFICATE-----\r\n") - 1) == 0;
}
