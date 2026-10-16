#include "video/hv_counter.h"
#include <assert.h>
#include <stdio.h>

/* A scanline is 3420 master cycles in both H32 and H40. */
#define LINE 3420u

static void check_mode(int h40, unsigned last_low, unsigned first_high)
{
    unsigned prev = vdp_h_counter(0, h40), zero_low7 = 0, i;
    int seen[256] = {0};
    assert(prev == 0x00);
    for (i = 0; i < LINE; i++) {
        unsigned h = vdp_h_counter(i, h40);
        seen[h] = 1;
        /* Counts up, with one jump from the end of the low run to the start of
         * the high run. */
        if (i)
            assert(h == prev || h == prev + 1 ||
                   (prev == last_low && h == first_high));
        if (!(h & 0x7Fu))
            zero_low7++;
        prev = h;
    }
    assert(prev == 0xFF);
    /* Every value of both runs appears, and none between them. */
    for (i = 0; i < 256; i++)
        assert(seen[i] == (i <= last_low || i >= first_high));
    /* Only $00 and $80 have the low 7 bits clear: about 2 counts in a line,
     * not every read (NHL 96 drains line energy when they are, #45). */
    assert(zero_low7 > 0 && zero_low7 * 50u < LINE);
    /* The line cycle wraps: the next line starts at 0 again. */
    assert(vdp_h_counter(LINE, h40) == 0x00);
}

int main(void)
{
    check_mode(1, 0xB6, 0xE4);   /* H40: $00-$B6, then $E4-$FF */
    check_mode(0, 0x93, 0xE9);   /* H32: $00-$93, then $E9-$FF */
    puts("hv_counter: ok");
    return 0;
}
