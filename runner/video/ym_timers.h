#ifndef GENESIS_YM_TIMERS_H
#define GENESIS_YM_TIMERS_H

#include <stdint.h>

/* YM2612 timers A and B, as the CPUs see them in the status byte.
 *
 * The chip itself is rendered later from the cycle-stamped write queue, so
 * this small synchronous model exists solely for status reads by polling
 * drivers. Times are absolute scheduler master cycles; the YM clock is
 * master / 7.
 *
 * - Timer A: 10-bit value TA from $24 (bits 9..2) and $25 (bits 1..0). One
 *   count per 144 YM clocks; overflow after (1024 - TA) counts. NHL 96's Z80
 *   driver clocks its DAC samples from it.
 * - Timer B: 8-bit value TB from $26. One count per 16 * 144 YM clocks;
 *   overflow after (256 - TB) counts. RKA's Z80 driver uses it as its music
 *   tick.
 * - $27: bit 0/1 load (run) A/B, bit 2/3 enable the A/B overflow flag, bit 4/5
 *   clear the A/B flag. A counter reloads only when its load bit goes from 0
 *   to 1, so re-writing $27 to acknowledge a flag leaves the count running.
 *   On overflow a running counter reloads from the current value, and raises
 *   its flag if the flag is enabled at that moment.
 *
 * BUSY is always clear. */
#define YM_TIMER_A_MASTER_TICK (144ull * 7ull)
#define YM_TIMER_B_MASTER_TICK (16ull * 144ull * 7ull)

typedef struct YmTimers {
    uint16_t ta;                /* timer A value, 10 bits ($24/$25)        */
    uint8_t  tb;                /* timer B value ($26)                     */
    uint8_t  mode;              /* last $27 write                          */
    uint8_t  status;            /* bit 0 = timer A flag, bit 1 = timer B   */
    uint64_t a_deadline;        /* next timer A overflow                   */
    uint64_t b_deadline;        /* next timer B overflow                   */
} YmTimers;

static inline uint64_t ym_timer_a_period(const YmTimers *t)
{
    return (uint64_t)(1024u - (t->ta & 0x3FFu)) * YM_TIMER_A_MASTER_TICK;
}

static inline uint64_t ym_timer_b_period(const YmTimers *t)
{
    return (uint64_t)(256u - t->tb) * YM_TIMER_B_MASTER_TICK;
}

/* Process one running counter's overflows up to `now`. A poll after several
 * periods leaves a single pending flag and the deadline on the original
 * cadence, without an unbounded catch-up loop. */
static inline void ym_timer_run(uint64_t *deadline, uint64_t period,
                                int flag_enabled, uint8_t *status,
                                uint8_t flag, uint64_t now)
{
    if (now < *deadline)
        return;
    if (flag_enabled)
        *status |= flag;
    *deadline += period;
    if (now >= *deadline)
        *deadline += ((now - *deadline) / period + 1u) * period;
}

static inline void ym_timers_update(YmTimers *t, uint64_t now)
{
    if (t->mode & 0x01u)
        ym_timer_run(&t->a_deadline, ym_timer_a_period(t), t->mode & 0x04u,
                     &t->status, 0x01u, now);
    if (t->mode & 0x02u)
        ym_timer_run(&t->b_deadline, ym_timer_b_period(t), t->mode & 0x08u,
                     &t->status, 0x02u, now);
}

static inline uint8_t ym_timers_status(YmTimers *t, uint64_t now)
{
    ym_timers_update(t, now);
    return t->status;
}

/* A write to a bank-0 register (`reg` is the latched address). */
static inline void ym_timers_write(YmTimers *t, uint8_t reg, uint8_t val,
                                   uint64_t now)
{
    ym_timers_update(t, now);
    if (reg == 0x24u) {
        t->ta = (uint16_t)((t->ta & 0x003u) | ((unsigned)val << 2));
    } else if (reg == 0x25u) {
        t->ta = (uint16_t)((t->ta & 0x3FCu) | (val & 0x03u));
    } else if (reg == 0x26u) {
        t->tb = val;
    } else if (reg == 0x27u) {
        if ((val & 0x01u) && !(t->mode & 0x01u))
            t->a_deadline = now + ym_timer_a_period(t);
        if ((val & 0x02u) && !(t->mode & 0x02u))
            t->b_deadline = now + ym_timer_b_period(t);
        t->status &= (uint8_t)~((val >> 4) & 0x03u);
        t->mode = val;
    }
}

#endif
