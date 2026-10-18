/* bus_expansion: host-memory regions above the cartridge and ROM read overlays.
 *
 * A game can map host buffers at 68K addresses a cartridge never uses
 * ($400000-$9FFFFF, open bus on a plain Genesis) and overlay a few ROM bytes
 * (for example to point one table entry at such a buffer) without modifying
 * the ROM image; RETROHUT#92. This checks the module the bus consults:
 * reads inside a region, outside it, across its end, overlays over ROM
 * words, the bus's fast-path span check, and what registration refuses. */
#include "bus_expansion.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const uint8_t data[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

    /* Nothing registered: every expansion read is open bus, ROM words pass
     * through unchanged. */
    bus_exp_reset();
    assert(!bus_exp_active());
    assert(bus_exp_read16(0x400000u) == 0xFFFFu);
    assert(bus_exp_rom16(0x002010u, 0x1357u) == 0x1357u);
    assert(!bus_exp_rom_hit(0x002010u));

    /* A region at $400000: word reads at even addresses, and bytes on both
     * lanes, come from the buffer; the word that straddles its end gets
     * open bus for the missing byte. */
    assert(bus_exp_map(0x400000u, data, sizeof data) == 0);
    assert(bus_exp_active());
    assert(bus_exp_read16(0x400000u) == 0x1122u);
    assert(bus_exp_read16(0x400004u) == 0x5566u);
    assert(bus_exp_read16(0x400006u) == 0xFFFFu);   /* past the end   */
    assert(bus_exp_read16(0x3FFFFEu) == 0xFFFFu);   /* below the start */
    assert(bus_exp_read16(0x500000u) == 0xFFFFu);   /* elsewhere      */

    /* A second region; overlapping or out-of-range ones are refused. */
    static const uint8_t more[2] = { 0xAB, 0xCD };
    assert(bus_exp_map(0x800000u, more, sizeof more) == 0);
    assert(bus_exp_read16(0x800000u) == 0xABCDu);
    assert(bus_exp_map(0x400004u, more, sizeof more) != 0);   /* overlaps   */
    assert(bus_exp_map(0x3FFFFEu, more, sizeof more) != 0);   /* in ROM     */
    assert(bus_exp_map(0x9FFFFFu, more, sizeof more) != 0);   /* past $9FFFFF */
    assert(bus_exp_map(0x401000u, more, 0) != 0);             /* empty      */
    assert(bus_exp_map(0x401001u, more, sizeof more) != 0);   /* odd base   */

    /* A ROM overlay: the long at $002010 (say, a pointer in a table) now
     * reads $00400000; the words around it read the ROM. */
    static const uint8_t ptr[4] = { 0x00, 0x40, 0x00, 0x00 };
    assert(bus_exp_overlay(0x002010u, ptr, sizeof ptr) == 0);
    assert(bus_exp_rom_hit(0x002010u) && bus_exp_rom_hit(0x002012u));
    assert(!bus_exp_rom_hit(0x00200Cu) && !bus_exp_rom_hit(0x002014u));
    assert(bus_exp_rom16(0x002010u, 0x0000u) == 0x0040u);
    assert(bus_exp_rom16(0x002012u, 0x1357u) == 0x0000u);
    assert(bus_exp_rom16(0x00200Eu, 0x2468u) == 0x2468u);
    assert(bus_exp_rom16(0x002014u, 0x9ABCu) == 0x9ABCu);

    /* An overlay covering half of a word replaces just that byte, and widens
     * the span the bus checks. */
    static const uint8_t one[1] = { 0x99 };
    assert(bus_exp_overlay(0x001001u, one, 1) == 0);
    assert(bus_exp_rom_hit(0x001000u));
    assert(bus_exp_rom16(0x001000u, 0x1234u) == 0x1299u);
    assert(bus_exp_overlay(0x3FFFFFu, ptr, sizeof ptr) != 0); /* leaves ROM */

    /* Switched off, overlays read as the ROM and the bus skips them; switched
     * back on, they apply again. Registering while off keeps them off. */
    bus_exp_overlays_enable(0);
    assert(!bus_exp_rom_hit(0x002010u));
    assert(bus_exp_rom16(0x002010u, 0x1357u) == 0x1357u);
    static const uint8_t two[2] = { 0x77, 0x88 };
    assert(bus_exp_overlay(0x003000u, two, sizeof two) == 0);
    assert(!bus_exp_rom_hit(0x003000u));
    assert(bus_exp_rom16(0x003000u, 0x1111u) == 0x1111u);
    bus_exp_overlays_enable(1);
    assert(bus_exp_rom_hit(0x002010u) && bus_exp_rom_hit(0x003000u));
    assert(bus_exp_rom16(0x002010u, 0x1357u) == 0x0040u);
    assert(bus_exp_rom16(0x003000u, 0x1111u) == 0x7788u);
    assert(bus_exp_overlay(0x002000u, ptr, 0) != 0);          /* empty      */

    /* Reset forgets everything. */
    bus_exp_reset();
    assert(!bus_exp_active());
    assert(bus_exp_read16(0x400000u) == 0xFFFFu);
    assert(!bus_exp_rom_hit(0x002010u));
    assert(bus_exp_rom16(0x002010u, 0x1357u) == 0x1357u);

    puts("bus_expansion: ok");
    return 0;
}
