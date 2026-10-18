/*
 * bus_expansion.c — expansion regions and ROM overlays (see bus_expansion.h).
 * Kept apart from genesis_bus.c so the bus fast path pays one flag test when
 * nothing is registered, and so the lookups can be tested on their own.
 */
#include "bus_expansion.h"
#include <string.h>

typedef struct { uint32_t base, size; const uint8_t *data; } ExpRegion;
typedef struct { uint32_t addr, len; uint8_t bytes[BUS_EXP_OVERLAY_MAX]; } ExpOverlay;

static ExpRegion  s_regions[BUS_EXP_REGIONS];
static ExpOverlay s_overlays[BUS_EXP_OVERLAYS];
static int        s_overlay_count;
static int        s_overlays_off;
static uint32_t   s_overlay_lo, s_overlay_hi;   /* span of every overlay */

int      g_bus_exp_regions;
uint32_t g_bus_exp_overlay_lo, g_bus_exp_overlay_hi;

int bus_exp_map(uint32_t base, const uint8_t *data, uint32_t size)
{
    if (!data || size == 0 || (base & 1u)) return -1;
    if (base < BUS_EXP_FIRST || base >= BUS_EXP_END || size > BUS_EXP_END - base)
        return -1;
    if (g_bus_exp_regions == BUS_EXP_REGIONS) return -1;
    for (int i = 0; i < g_bus_exp_regions; i++) {
        const ExpRegion *r = &s_regions[i];
        if (base < r->base + r->size && r->base < base + size) return -1;
    }
    s_regions[g_bus_exp_regions] = (ExpRegion){ base, size, data };
    g_bus_exp_regions++;
    return 0;
}

int bus_exp_overlay(uint32_t addr, const uint8_t *bytes, uint32_t len)
{
    if (!bytes || len == 0 || len > BUS_EXP_OVERLAY_MAX) return -1;
    if (addr >= BUS_EXP_FIRST || len > BUS_EXP_FIRST - addr) return -1;
    if (s_overlay_count == BUS_EXP_OVERLAYS) return -1;
    ExpOverlay *o = &s_overlays[s_overlay_count];
    o->addr = addr;
    o->len  = len;
    memcpy(o->bytes, bytes, len);
    s_overlay_count++;
    if (s_overlay_hi == 0 || addr < s_overlay_lo) s_overlay_lo = addr;
    if (addr + len > s_overlay_hi) s_overlay_hi = addr + len;
    bus_exp_overlays_enable(!s_overlays_off);
    return 0;
}

void bus_exp_overlays_enable(int on)
{
    s_overlays_off = !on;
    /* The bus tests this span first; an empty one ([0, 0)) skips overlays. */
    g_bus_exp_overlay_lo = on ? s_overlay_lo : 0;
    g_bus_exp_overlay_hi = on ? s_overlay_hi : 0;
}

void bus_exp_reset(void)
{
    g_bus_exp_regions = 0;
    s_overlay_count = 0;
    s_overlays_off = 0;
    s_overlay_lo = s_overlay_hi = 0;
    g_bus_exp_overlay_lo = g_bus_exp_overlay_hi = 0;
}

int bus_exp_active(void)
{
    return g_bus_exp_regions | s_overlay_count;
}

static uint8_t region_byte(uint32_t a)
{
    for (int i = 0; i < g_bus_exp_regions; i++) {
        const ExpRegion *r = &s_regions[i];
        if (a - r->base < r->size) return r->data[a - r->base];
    }
    return 0xFFu;                                   /* open bus */
}

uint16_t bus_exp_read16(uint32_t a)
{
    return (uint16_t)((region_byte(a) << 8) | region_byte(a + 1u));
}

/* Later overlays win where two cover the same byte. */
static int overlay_byte(uint32_t a, uint8_t *out)
{
    int hit = 0;
    for (int i = 0; i < s_overlay_count; i++) {
        const ExpOverlay *o = &s_overlays[i];
        if (a - o->addr < o->len) { *out = o->bytes[a - o->addr]; hit = 1; }
    }
    return hit;
}

uint16_t bus_exp_rom16(uint32_t a, uint16_t rom_word)
{
    if (s_overlays_off) return rom_word;
    uint8_t hi = (uint8_t)(rom_word >> 8), lo = (uint8_t)rom_word;
    overlay_byte(a, &hi);
    overlay_byte(a + 1u, &lo);
    return (uint16_t)((hi << 8) | lo);
}
