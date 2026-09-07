#pragma once
#include <stdbool.h>
#include <stddef.h>

// Kconfig strings cannot contain physical line breaks. Decode the escaped
// representation once, after Kconfig and the C compiler have processed it.
bool wireless_certificate_decode(const char *input, char *output, size_t capacity);
