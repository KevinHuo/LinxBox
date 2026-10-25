#include "linx_wifi.h"

/* 有界检查供网页和后端使用，避免未终止字符串进入驱动或日志。 */
static bool password_length(const char *password, size_t *length)
{
    if (password == NULL) return false;
    size_t size = 0;
    while (size < LINX_WIFI_PASSWORD_CAPACITY && password[size] != '\0') {
        unsigned char character = (unsigned char)password[size];
        if (character < 32U || character > 126U) return false;
        ++size;
    }
    if (size == LINX_WIFI_PASSWORD_CAPACITY) return false;
    *length = size;
    return true;
}

bool linx_wifi_password_valid(const char *password, bool secure)
{
    size_t length;
    if (!password_length(password, &length)) return false;
    if (!secure) return length == 0U;
    if (length == 64U) {
        for (size_t i = 0; i < length; ++i) {
            char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }
    return length >= 8U;
}
