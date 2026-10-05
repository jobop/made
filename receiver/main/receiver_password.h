#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RECEIVER_PASSWORD_LENGTH 8
#define RECEIVER_PASSWORD_VERSION 2

// Reject the incomplete bucket so all eight-digit values are equally likely.
// Keep leading zeroes: WPA2 requires all eight characters to be entered.
static inline bool receiver_password_from_random(uint32_t random_value,
                                                 char output[RECEIVER_PASSWORD_LENGTH + 1])
{
    if (random_value >= UINT32_C(4200000000)) return false;
    uint32_t value = random_value % UINT32_C(100000000);
    for (int i = RECEIVER_PASSWORD_LENGTH - 1; i >= 0; --i) {
        output[i] = (char)('0' + value % 10);
        value /= 10;
    }
    output[RECEIVER_PASSWORD_LENGTH] = '\0';
    return true;
}

// Earlier firmware had no version marker or custom-password UI and generated
// exactly 16 characters from this alphabet. Preserve all other saved values,
// including every versioned password and already-short numeric passwords.
static inline bool receiver_password_needs_migration(const char *password, bool version_present)
{
    static const char legacy_alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    return !version_present && password != NULL && strlen(password) == 16 &&
           strspn(password, legacy_alphabet) == 16;
}
