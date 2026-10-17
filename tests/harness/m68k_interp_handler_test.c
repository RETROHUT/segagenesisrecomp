/* m68k_interp_run_handler: where an interpreted interrupt handler ends.
 *
 * The handler capsule ends at the handler's own RTE, which pops SR and PC as
 * generated code's RTE does (the scheduler restores the interrupted state
 * afterwards). Calls inside the handler nest (JSR/BSR ... RTS), and an RTS
 * with nothing of the handler's own to return from is a computed jump: a
 * vector that pushes the real handler's address and RTSes to it
 * (MOVE.L (ptr).L,-(SP); RTS, with the pointer in RAM) must still end at
 * that handler's RTE.
 * Before, the vector's RTS made the depth negative, the handler's RTE wasn't
 * recognized, and the interpreter ran on into the interrupted code inside
 * the handler, which never yields (RETROHUT#14). */
#include "m68k_interp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

M68KState g_cpu;
uint8_t   g_rom[0x400000];
uint8_t   g_ram[0x010000];
uint64_t  g_native_insn_count;
uint32_t  g_cycle_accumulator, g_audio_cycle_counter, g_vblank_threshold = 0xFFFFFFFFu;
uint64_t  g_cosim_cycle, g_cosim_next_cp;
static int s_rte_pending;
int      *g_rte_pending_ptr = &s_rte_pending;

static uint8_t *at(uint32_t a)
{
    a &= 0xFFFFFFu;
    return a >= 0xFF0000u ? &g_ram[a & 0xFFFFu] : &g_rom[a];
}
uint8_t  m68k_read8(uint32_t a)  { return *at(a); }
uint16_t m68k_read16(uint32_t a) { return (uint16_t)(*at(a) << 8 | *at(a + 1)); }
uint32_t m68k_read32(uint32_t a) { return (uint32_t)m68k_read16(a) << 16 | m68k_read16(a + 2); }
void m68k_write8(uint32_t a, uint8_t v)   { *at(a) = v; }
void m68k_write16(uint32_t a, uint16_t v) { *at(a) = (uint8_t)(v >> 8); *at(a + 1) = (uint8_t)v; }
void m68k_write32(uint32_t a, uint32_t v) { m68k_write16(a, (uint16_t)(v >> 16)); m68k_write16(a + 2, (uint16_t)v); }
int  game_instruction_hook_site(uint32_t pc)    { (void)pc; return 0; }
int  genesis_game_instruction_hook(uint32_t pc) { (void)pc; return 0; }
void glue_check_vblank(void) {}

static void put16(uint32_t a, uint16_t v) { m68k_write16(a, v); }
static void put32(uint32_t a, uint32_t v) { m68k_write32(a, v); }

/* An interrupted context: an exception frame (SR, then PC) on the stack. */
static void enter(void)
{
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.SR = 0x2600;
    g_cpu.A[7] = 0xFFFE00u;
    g_cpu.A[7] -= 4; put32(g_cpu.A[7], 0x00ABCDEEu);   /* interrupted PC */
    g_cpu.A[7] -= 2; put16(g_cpu.A[7], 0x2000);        /* interrupted SR */
}

int main(void)
{
    /* Handler body at $1000: BSR to $1100, MOVEQ #1,D0, RTE.
     * $1100: MOVEQ #2,D1; RTS. */
    put16(0x1000, 0x6100); put16(0x1002, 0x00FE);      /* BSR.W $1100  */
    put16(0x1004, 0x7001);                             /* MOVEQ #1,D0  */
    put16(0x1006, 0x4E73);                             /* RTE          */
    put16(0x1100, 0x7202);                             /* MOVEQ #2,D1  */
    put16(0x1102, 0x4E75);                             /* RTS          */

    /* Direct vector: ends at the RTE, which pops the frame. */
    enter();
    assert(m68k_interp_run_handler(0x1000) == M68KI_OK);
    assert(g_cpu.D[0] == 1 && g_cpu.D[1] == 2);
    assert(g_cpu.PC == 0x00ABCDEEu);
    assert(g_cpu.A[7] == 0xFFFE00u);
    assert(g_cpu.SR == 0x2000);

    /* Trampoline vector at $800: MOVE.L ($FF8000).L,-(SP); RTS, with the
     * handler's address in RAM. Same end. */
    put16(0x0800, 0x2F39); put32(0x0802, 0x00FF8000u);  /* MOVE.L ($FF8000).L,-(SP) */
    put16(0x0806, 0x4E75);                              /* RTS                      */
    put32(0xFF8000u, 0x00001000u);
    enter();
    assert(m68k_interp_run_handler(0x0800) == M68KI_OK);
    assert(g_cpu.D[0] == 1 && g_cpu.D[1] == 2);
    assert(g_cpu.PC == 0x00ABCDEEu);
    assert(g_cpu.A[7] == 0xFFFE00u);
    assert(g_cpu.SR == 0x2000);

    puts("m68k_interp_handler: ok");
    return 0;
}
