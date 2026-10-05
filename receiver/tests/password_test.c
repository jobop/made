#include <assert.h>
#include <stdio.h>
#include "../main/receiver_password.h"

int main(void)
{
    char password[RECEIVER_PASSWORD_LENGTH + 1];
    assert(receiver_password_from_random(0, password));
    assert(strcmp(password, "00000000") == 0);
    assert(receiver_password_from_random(42, password));
    assert(strcmp(password, "00000042") == 0);
    assert(receiver_password_from_random(UINT32_C(4199999999), password));
    assert(strcmp(password, "99999999") == 0);
    assert(!receiver_password_from_random(UINT32_C(4200000000), password));
    assert(!receiver_password_from_random(UINT32_MAX, password));
    assert(strcmp(password, "99999999") == 0);
    for (uint32_t value = 0; value < 100000; ++value) {
        assert(receiver_password_from_random(value * 41999, password));
        assert(strlen(password) == 8);
        assert(strspn(password, "0123456789") == 8);
    }
    assert(receiver_password_needs_migration("ABCDEFGH23456789", false));
    assert(!receiver_password_needs_migration("ABCDEFGH23456789", true));
    assert(!receiver_password_needs_migration("custom-password!", false));
    assert(!receiver_password_needs_migration("ABCDEFGI23456789", false));
    assert(!receiver_password_needs_migration("12345678", false));
    assert(!receiver_password_needs_migration("", false));
    assert(!receiver_password_needs_migration(NULL, false));
    // A migration interrupted after password persistence but before the marker
    // must not generate a second password; later restarts preserve it as well.
    assert(receiver_password_from_random(12345678, password));
    assert(!receiver_password_needs_migration(password, false));
    assert(!receiver_password_needs_migration(password, true));
    puts("password generation and migration checks passed");
    return 0;
}
