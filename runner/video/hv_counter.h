#ifndef GENESIS_HV_COUNTER_H
#define GENESIS_HV_COUNTER_H

#include <stdint.h>

/* The VDP H counter: the low byte of the HV counter at $C00008.
 *
 * A scanline is 3420 master cycles in both modes. The counter steps once per
 * two pixels, so a line has 171 counts in H32 (342 pixels) and 211 in H40
 * (about 420 pixels), and the values are not contiguous: H32 counts $00-$93
 * and then jumps to $E9-$FF, H40 counts $00-$B6 and then $E4-$FF.
 *
 * `line_cycle` is the master-cycle position in the current line. Counts are
 * spread evenly over the line; the real dot clock slows during H-sync in
 * H40, which this ignores, so the value is right to within a few counts.
 * Games that read it as a cheap random number (NHL 96 drains line energy
 * only when the low 7 bits are 0, #45) see the hardware's spread. */
#define VDP_LINE_MASTER_CYCLES 3420u

static inline unsigned vdp_h_counter(uint32_t line_cycle, int h40)
{
    unsigned counts    = h40 ? 211u : 171u;
    unsigned last_low  = h40 ? 0xB6u : 0x93u;
    unsigned first_high = h40 ? 0xE4u : 0xE9u;
    unsigned i = (unsigned)((line_cycle % VDP_LINE_MASTER_CYCLES) * counts /
                            VDP_LINE_MASTER_CYCLES);
    return i <= last_low ? i : first_high + (i - last_low - 1u);
}

#endif
