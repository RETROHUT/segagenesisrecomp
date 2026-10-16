#include "video/ym_timers.h"
#include <assert.h>
#include <stdio.h>

/* One timer-A count is 144 YM clocks, one timer-B count 16 * 144; the YM clock
 * is master / 7. */
#define TA_TICK (144ull * 7ull)
#define TB_TICK (16ull * 144ull * 7ull)

static void write_ta(YmTimers *t, unsigned ta, uint64_t now)
{
    ym_timers_write(t, 0x24, (uint8_t)(ta >> 2), now);
    ym_timers_write(t, 0x25, (uint8_t)(ta & 3u), now);
}

static int flag_a(YmTimers *t, uint64_t now) { return ym_timers_status(t, now) & 1; }
static int flag_b(YmTimers *t, uint64_t now) { return (ym_timers_status(t, now) >> 1) & 1; }

int main(void)
{
    YmTimers t = {0};
    uint64_t now = 1000;

    /* Timer A: 10-bit value from $24 (high 8) and $25 (low 2); it overflows
     * after (1024 - TA) counts. Load (bit 0) and flag enable (bit 2). */
    write_ta(&t, 1000, now);
    ym_timers_write(&t, 0x27, 0x05, now);
    uint64_t period = (1024 - 1000) * TA_TICK;
    assert(!flag_a(&t, now + period - 1));
    assert(flag_a(&t, now + period));
    assert(!flag_b(&t, now + period));

    /* Bit 4 clears the flag; writing it with the load bit still set does not
     * restart the count, so the next overflow stays on the original cadence.
     * (NHL 96's sample player acknowledges every overflow this way.) */
    ym_timers_write(&t, 0x27, 0x15, now + period + 5);
    assert(!flag_a(&t, now + period + 5));
    assert(!flag_a(&t, now + 2 * period - 1));
    assert(flag_a(&t, now + 2 * period));

    /* The counter keeps running with the flag disabled; no flag is raised. */
    ym_timers_write(&t, 0x27, 0x11, now + 2 * period);
    assert(!flag_a(&t, now + 4 * period));

    /* Stopping (bit 0 clear) and loading again restarts from the load time. */
    ym_timers_write(&t, 0x27, 0x00, now + 4 * period);
    uint64_t start = now + 4 * period + 77;
    ym_timers_write(&t, 0x27, 0x05, start);
    assert(!flag_a(&t, start + period - 1));
    assert(flag_a(&t, start + period));

    /* A new TA value takes effect at the next reload, not mid-count. */
    write_ta(&t, 1020, start + period);
    ym_timers_write(&t, 0x27, 0x15, start + period);
    assert(!flag_a(&t, start + 2 * period - 1));
    assert(flag_a(&t, start + 2 * period));
    ym_timers_write(&t, 0x27, 0x15, start + 2 * period);
    assert(!flag_a(&t, start + 2 * period + 4 * TA_TICK - 1));
    assert(flag_a(&t, start + 2 * period + 4 * TA_TICK));

    /* Timer B: $26, overflow after (256 - TB) counts of 16 * 144 YM clocks.
     * It runs independently of timer A, and re-writing $27 with its load bit
     * already set does not restart it either. */
    YmTimers u = {0};
    ym_timers_write(&u, 0x26, 0xF2, 0);
    ym_timers_write(&u, 0x27, 0x0A, 0);
    uint64_t pb = (256 - 0xF2) * TB_TICK;
    ym_timers_write(&u, 0x27, 0x2A, pb / 2);
    assert(!flag_b(&u, pb - 1));
    assert(flag_b(&u, pb));
    assert(!flag_a(&u, pb));

    /* Stop/clear then load (RKA writes $3C, updates $26, writes $3A). */
    ym_timers_write(&u, 0x27, 0x3C, pb + 10);
    assert(!flag_b(&u, pb + 10));
    ym_timers_write(&u, 0x26, 0xF0, pb + 10);
    ym_timers_write(&u, 0x27, 0x3A, pb + 10);
    uint64_t pb2 = (256 - 0xF0) * TB_TICK;
    assert(!flag_b(&u, pb + 10 + pb2 - 1));
    assert(flag_b(&u, pb + 10 + pb2));

    /* Both timers together, as NHL 96 runs them ($27 = $0F, then $1F for
     * each acknowledged timer-A overflow). */
    YmTimers v = {0};
    write_ta(&v, 1000, 0);
    ym_timers_write(&v, 0x26, 0xF2, 0);
    ym_timers_write(&v, 0x27, 0x0F, 0);
    int overflows = 0;
    for (uint64_t c = 0; c < pb; c += 64) {
        if (flag_a(&v, c)) { overflows++; ym_timers_write(&v, 0x27, 0x1F, c); }
    }
    assert(overflows == (int)((pb - 1) / period));
    assert(flag_b(&v, pb));

    /* A long gap between polls leaves one pending flag, not a backlog. */
    YmTimers w = {0};
    write_ta(&w, 1000, 0);
    ym_timers_write(&w, 0x27, 0x05, 0);
    assert(flag_a(&w, 100 * period + 3));
    ym_timers_write(&w, 0x27, 0x15, 100 * period + 3);
    assert(!flag_a(&w, 101 * period - 1));
    assert(flag_a(&w, 101 * period));

    puts("ym_timers: ok");
    return 0;
}
