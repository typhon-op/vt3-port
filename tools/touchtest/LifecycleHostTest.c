/* Host checks for the guest arena lifecycle a relaunch depends on. */
#include "vt3/vt3_guest.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    int checks = 0, fails = 0;
#define OK(c, w) do { checks++; if (!(c)) { fails++; printf("FAIL: %s\n", w); } } while (0)
    size_t bytes = 64u << 20;
    OK(vt3_guest_init(bytes) == 0, "arena maps");
    uint8_t *first = vt3_guest_base_;
    OK(first != NULL && vt3_guest_size() == bytes, "arena size reported");
    memset(first, 0xAB, 4096);
    OK(vt3_guest_init(bytes) == 0 && vt3_guest_base_ == first, "second init reuses the mapping (no second arena)");
    vt3_guest_shutdown();
    OK(vt3_guest_base_ == NULL && vt3_guest_size() == 0, "shutdown unmaps and clears the globals");
    OK(vt3_guest_init(bytes) == 0, "arena maps again for the next match");
    OK(vt3_guest_base_ != NULL, "arena has a base after relaunch");
    int leftover = 0;
    for (size_t i = 0; i < 4096; i++) if (vt3_guest_base_[i] != 0) { leftover = 1; break; }
    OK(!leftover, "a remapped arena carries no bytes from the previous match");
    vt3_guest_shutdown();
    OK(vt3_guest_base_ == NULL, "shutdown is idempotent");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
