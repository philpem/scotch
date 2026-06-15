/* Regression test for the ARM sandbox (replay/armsim.h).
 *
 * Asserts the two classic-ARM memory behaviours the sandbox exists to model
 * correctly -- the behaviours Unicorn (a much later core) gets wrong:
 *   - an unaligned LDR rotates the loaded word by the byte offset, and
 *   - an unaligned LDM/word load ignores the low two address bits.
 */

#include "replay/armsim.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CODE 0x00100000u
#define DATA 0x00200000u
#define RET  0x04000000u

static int failures;

static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %-30s got=0x%08x want=0x%08x\n", what, got, want);
        failures++;
    } else {
        printf("ok   %-30s = 0x%08x\n", what, got);
    }
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

int main(void)
{
    ReplayArmSim *sim = replay_armsim_new(REPLAY_ARM_MODE_32);
    unsigned char code[32];
    unsigned char data[8];
    unsigned char ret[4];
    uint64_t executed = 0;
    ReplayArmStatus st;

    if (sim == NULL) {
        printf("replay_armsim_new failed\n");
        return 1;
    }

    memset(code, 0, sizeof code);
    put32(code + 0, 0xE3A0002Au);                    /* MOV   r0, #42      */
    put32(code + 4, 0xE5921000u);                    /* LDR   r1, [r2]     */
    put32(code + 8, 0xE8930010u);                    /* LDMIA r3, {r4}     */
    put32(code + 12, replay_armsim_return_opcode()); /* SWI sentinel       */
    replay_armsim_map(sim, CODE, code, sizeof code, sizeof code);

    put32(data + 0, 0x11223344u);
    put32(data + 4, 0x55667788u);
    replay_armsim_map(sim, DATA, data, sizeof data, sizeof data);

    put32(ret, replay_armsim_return_opcode());
    replay_armsim_map(sim, RET, ret, sizeof ret, sizeof ret);

    replay_armsim_set_reg(sim, 2, DATA + 1);   /* unaligned LDR base */
    replay_armsim_set_reg(sim, 3, DATA + 1);   /* unaligned LDM base */
    replay_armsim_set_reg(sim, 13, 0x03000000u);
    replay_armsim_set_reg(sim, 14, RET);

    st = replay_armsim_run(sim, CODE, 1000, &executed);

    check("status OK", (uint32_t)st, (uint32_t)REPLAY_ARM_OK);
    check("r0 (MOV)", replay_armsim_get_reg(sim, 0), 42u);
    check("r1 (unaligned LDR rotate)", replay_armsim_get_reg(sim, 1),
          0x44112233u);
    check("r4 (unaligned LDM ignores lsb)", replay_armsim_get_reg(sim, 4),
          0x11223344u);

    replay_armsim_free(sim);

    if (failures != 0) {
        printf("\n%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("\nALL OK\n");
    return 0;
}
