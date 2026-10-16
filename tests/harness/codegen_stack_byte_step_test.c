/*
 * codegen_stack_byte_step_test.c — byte-sized -(A7) and (A7)+ step by 2.
 *
 * On the 68000 a byte-sized predecrement or postincrement on A7 moves it by
 * 2, so the stack stays word-aligned; a byte access through any other An
 * moves it by 1. This recompiles one synthetic function covering the load and store
 * paths, plain and read-modify-write, and checks the emitted C.
 *
 * argv[1] is a scratch directory for the generated files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "annotations.h"
#include "code_generator.h"
#include "function_finder.h"
#include "game_config.h"
#include "rom_parser.h"

#define FUNC_ADDR 0x000200u

static const uint16_t k_func[] = {
    0x1F00, /* MOVE.B D0,-(A7)   store, predec      */
    0x101F, /* MOVE.B (A7)+,D0   load, postinc      */
    0x1027, /* MOVE.B -(A7),D0   load, predec       */
    0x1EC0, /* MOVE.B D0,(A7)+   store, postinc     */
    0x4227, /* CLR.B -(A7)       store, predec      */
    0x521F, /* ADDQ.B #1,(A7)+   RMW, postinc       */
    0x4427, /* NEG.B -(A7)       RMW, predec        */
    0x1100, /* MOVE.B D0,-(A0)   A0 still steps by 1 */
    0x1018, /* MOVE.B (A0)+,D0                       */
    0x4E75, /* RTS                                   */
};

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

static int count(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

/* Concatenates every generated part file. */
static char *read_parts(const char *dir) {
    size_t len = 0, cap = 1 << 16;
    char *all = malloc(cap);
    if (!all) return NULL;
    for (int k = 0; k < GENESIS_SPLIT_PART_COUNT; k++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/stackstep_part%02d.c", dir, k);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            while (len + n + 1 > cap) {
                char *grown = realloc(all, cap *= 2);
                if (!grown) { free(all); fclose(f); return NULL; }
                all = grown;
            }
            memcpy(all + len, buf, n);
            len += n;
        }
        fclose(f);
    }
    all[len] = '\0';
    return all;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    static uint8_t rom_bytes[0x400];
    for (size_t i = 0; i < sizeof(k_func) / sizeof(k_func[0]); i++) {
        rom_bytes[FUNC_ADDR + 2 * i]     = (uint8_t)(k_func[i] >> 8);
        rom_bytes[FUNC_ADDR + 2 * i + 1] = (uint8_t)k_func[i];
    }
    GenesisRom rom;
    memset(&rom, 0, sizeof(rom));
    rom.rom_data = rom_bytes;
    rom.rom_size = sizeof(rom_bytes);

    FunctionEntry entry = { FUNC_ADDR, FUNC_EVIDENCE_CONFIG };
    FunctionList funcs = { &entry, 1, 1 };
    AnnotationTable at = { NULL, 0 };
    GameConfig cfg;
    game_config_init_empty(&cfg);

    char dispatch_path[1024];
    snprintf(dispatch_path, sizeof(dispatch_path), "%s/stackstep_dispatch.c", dir);
    if (!codegen_emit(&rom, &funcs, dir, "stackstep", dispatch_path, &at, &cfg, false)) {
        fprintf(stderr, "FAIL codegen_emit\n");
        return 1;
    }
    char *c = read_parts(dir);
    if (!c) {
        fprintf(stderr, "FAIL reading generated parts\n");
        return 1;
    }

    CHECK(count(c, "g_cpu.A[7] -= 1") == 0, "no -(A7) byte step of 1");
    CHECK(count(c, "g_cpu.A[7] += 1") == 0, "no (A7)+ byte step of 1");
    CHECK(count(c, "g_cpu.A[7] -= 2") == 4, "-(A7) byte steps by 2 (MOVE x2, CLR, NEG)");
    CHECK(count(c, "g_cpu.A[7] += 2") == 3, "(A7)+ byte steps by 2 (MOVE x2, ADDQ)");
    CHECK(count(c, "g_cpu.A[0] -= 1") == 1, "-(A0) byte still steps by 1");
    CHECK(count(c, "g_cpu.A[0] += 1") == 1, "(A0)+ byte still steps by 1");

    free(c);
    game_config_free(&cfg);
    if (failures) {
        fprintf(stderr, "codegen_stack_byte_step_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("codegen_stack_byte_step_test: all tests passed");
    return 0;
}
