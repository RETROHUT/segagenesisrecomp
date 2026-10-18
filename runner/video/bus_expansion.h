/*
 * bus_expansion.h — game-supplied memory the 68K bus serves besides the
 * cartridge (RETROHUT#92).
 *
 * Two opt-in mechanisms, registered by a game at startup (GameSpec.bus_setup)
 * and read-only from the 68000:
 *   - expansion regions: host buffers mapped at fixed addresses in
 *     $400000-$9FFFFF, which a plain Genesis leaves as open bus;
 *   - ROM overlays: a few ROM bytes that reads return instead of the ROM's,
 *     for example to point one table entry at an expansion region. The ROM
 *     image itself is never modified.
 *
 * With nothing registered, every read behaves exactly as before. The data is
 * static, registered once at startup before the game runs (registration is
 * not thread-safe), so it isn't part of save states.
 *
 * Both apply to reads through the bus (gbus_read16 and what goes through it:
 * the CPU's data reads, VDP DMA, the Z80 bank window, debug peeks). They
 * don't apply to instruction fetches the interpreter makes straight from
 * g_rom, to the reset vector, or to anything the recompiler read from the ROM
 * at build time, so an overlay is for data, not code.
 */
#ifndef BUS_EXPANSION_H
#define BUS_EXPANSION_H

#include <stdint.h>

#define BUS_EXP_FIRST   0x400000u   /* lowest address a region may use   */
#define BUS_EXP_END     0xA00000u   /* regions end before this (Z80 area) */
#define BUS_EXP_REGIONS 8           /* most regions a game can map        */
#define BUS_EXP_OVERLAYS 32         /* most overlays a game can register  */
#define BUS_EXP_OVERLAY_MAX 16      /* bytes per overlay                  */

/* Map `size` bytes of `data` at 68K address `base` (even, inside
 * [BUS_EXP_FIRST, BUS_EXP_END), not overlapping another region). The buffer
 * is used in place and must outlive the session. Returns 0, or -1 if
 * refused. */
int bus_exp_map(uint32_t base, const uint8_t *data, uint32_t size);

/* Make ROM reads of [addr, addr + len) return `bytes` (copied). The range
 * must lie inside the ROM area (below BUS_EXP_FIRST) and len must be 1 to
 * BUS_EXP_OVERLAY_MAX. Returns 0, or -1 if refused. Overlays apply once
 * registered, unless switched off below. */
int bus_exp_overlay(uint32_t addr, const uint8_t *bytes, uint32_t len);

/* Switch every overlay on (1) or off (0) without forgetting them. A game that
 * sums its ROM at boot sees an overlay as a bad checksum (NHL 96 shows its
 * red screen), so it registers its overlays off and switches them on once the
 * check is behind it, for example from its GameSpec.instruction_hook at a
 * site past the check. */
void bus_exp_overlays_enable(int on);

/* Forget every region and overlay (tests; a game never needs it). */
void bus_exp_reset(void);

/* Anything registered at all? */
int bus_exp_active(void);

/* For the bus's fast path, kept current by the functions above: the number of
 * regions, and the span [lo, hi) that overlays cover (hi = 0: none). */
extern int      g_bus_exp_regions;
extern uint32_t g_bus_exp_overlay_lo, g_bus_exp_overlay_hi;

/* The word at `a` and `a + 1` in $400000-$9FFFFF: region bytes where mapped,
 * $FF (open bus) elsewhere. */
uint16_t bus_exp_read16(uint32_t a);

/* The ROM word `rom_word` read at `a`, with any overlaid bytes substituted. */
uint16_t bus_exp_rom16(uint32_t a, uint16_t rom_word);

/* Does the word at `a` touch an overlay's span? Cheap enough for every ROM
 * read. */
static inline int bus_exp_rom_hit(uint32_t a)
{
    return a + 1u >= g_bus_exp_overlay_lo && a < g_bus_exp_overlay_hi;
}

#endif
