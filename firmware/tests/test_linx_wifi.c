#include "linx_wifi.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_password_validation(void)
{
    assert(!linx_wifi_password_valid(NULL, false));
    assert(!linx_wifi_password_valid(NULL, true));
    assert(linx_wifi_password_valid("", false));
    assert(!linx_wifi_password_valid("a", false));
    assert(!linx_wifi_password_valid("", true));
    assert(!linx_wifi_password_valid("1234567", true));
    assert(linx_wifi_password_valid("12345678", true));
    assert(linx_wifi_password_valid(" aZ0!\\\" ", true));
    char password[66];
    memset(password, ' ', 63);
    password[63] = '\0';
    assert(linx_wifi_password_valid(password, true));
    assert(!linx_wifi_password_valid(password, false));
    memset(password, 'g', 64);
    password[64] = '\0';
    assert(!linx_wifi_password_valid(password, true));
    const char *hex = "0123456789abcdefABCDEF";
    for (size_t i = 0; i < 64; ++i) password[i] = hex[i % strlen(hex)];
    assert(linx_wifi_password_valid(password, true));
    assert(!linx_wifi_password_valid(password, false));
    password[63] = '/';
    assert(!linx_wifi_password_valid(password, true));
    password[63] = ':';
    assert(!linx_wifi_password_valid(password, true));
    password[63] = '@';
    assert(!linx_wifi_password_valid(password, true));
    password[63] = 'G';
    assert(!linx_wifi_password_valid(password, true));
    password[63] = '`';
    assert(!linx_wifi_password_valid(password, true));
    password[63] = 'f';
    assert(linx_wifi_password_valid(password, true));
    password[64] = 'a';
    password[65] = '\0';
    assert(!linx_wifi_password_valid(password, true));
    assert(!linx_wifi_password_valid(password, false));
    memset(password, 'a', sizeof(password));
    assert(!linx_wifi_password_valid(password, true));
    for (unsigned character = 1; character < 256; ++character) {
        memset(password, 'a', 8);
        password[0] = (char)character;
        password[8] = '\0';
        assert(linx_wifi_password_valid(password, true) == (character >= 32 && character <= 126));
    }
    assert(!linx_wifi_password_valid("含中文密码", true));
}

int main(void)
{
    test_password_validation();
    puts("Wi-Fi 密码验证：长度、可打印字符、HEX、开放网络与非法输入通过");
    return 0;
}
