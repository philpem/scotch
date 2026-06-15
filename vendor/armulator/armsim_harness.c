/* armsim_harness.c -- replay-tooling glue around the vendored ARMulator.
 *
 * This file is NOT part of the pristine ARM Ltd / GDB sources; it is the
 * replay-tooling boundary layer that implements the public replay/armsim.h API
 * on top of the vendored emulator, and supplies the handful of host-environment
 * symbols the emulator references (the GDB simulator framework normally provides
 * them). It is compiled with the same relaxed warnings as the vendored code
 * because it includes the vendored headers.
 */

#include "armdefs.h"
#include "armemu.h"

#include "replay/armsim.h"

#include <stdarg.h>

/* SWI comment field used as the "execution finished" sentinel. The opcode
 * placed at the return address is a plain ARM SWI carrying this number. */
#define REPLAY_ARMSIM_SWI_RETURN 0xC0FFEEu

struct ReplayArmSim {
    ARMul_State *state;
    ReplayArmMode mode;
    int returned;  /* set by ARMul_OSHandleSWI on the sentinel SWI */
};

/* ------------------------------------------------------------------ */
/* Host-environment symbols the vendored emulator expects.            */
/* ------------------------------------------------------------------ */

/* Referenced by the emulation loop / sim framework. */
int stop_simulator = 0;
int trace = 0;
int disas = 0;
int trace_funcs = 0;

/* Disassembly hook (only reached when `disas`/`trace` are set, which we never
 * do). */
void
print_insn (ARMword instr ATTRIBUTE_UNUSED)
{
}

/* XScale/CP15 helpers. These are called from abort and memory paths but only
 * do anything when state->is_XScale is set, which this harness never selects;
 * they exist purely so the vendored objects link. */
void
XScale_set_fsr_far (ARMul_State * state ATTRIBUTE_UNUSED,
                    ARMword fsr ATTRIBUTE_UNUSED,
                    ARMword far_addr ATTRIBUTE_UNUSED)
{
}

void
XScale_check_memacc (ARMul_State * state ATTRIBUTE_UNUSED,
                     ARMword * address ATTRIBUTE_UNUSED,
                     int store ATTRIBUTE_UNUSED)
{
}

int
XScale_debug_moe (ARMul_State * state ATTRIBUTE_UNUSED,
                  int moe ATTRIBUTE_UNUSED)
{
    return 0;
}

ARMword
read_cp15_reg (unsigned reg ATTRIBUTE_UNUSED,
               unsigned opcode_2 ATTRIBUTE_UNUSED,
               unsigned CRm ATTRIBUTE_UNUSED)
{
    return 0;
}

void
ARMul_ConsolePrint (ARMul_State * state ATTRIBUTE_UNUSED,
                    const char * format ATTRIBUTE_UNUSED, ...)
{
    /* The standalone harness has no console; swallow the emulator's chatter. */
}

/* No coprocessors are modelled: leave the dispatch tables empty so any
 * coprocessor instruction (the codecs issue none) decodes as undefined. */
unsigned
ARMul_CoProInit (ARMul_State * state ATTRIBUTE_UNUSED)
{
    return TRUE;
}

void
ARMul_CoProExit (ARMul_State * state ATTRIBUTE_UNUSED)
{
}

/* iWMMXt is never enabled; this only needs to resolve at link time. */
int
ARMul_HandleIwmmxt (ARMul_State * state ATTRIBUTE_UNUSED,
                    ARMword instr ATTRIBUTE_UNUSED)
{
    return 0;
}

/* Thumb is never entered (the codecs are pure ARM); resolve the symbol only. */
tdstate
ARMul_ThumbDecode (ARMul_State * state ATTRIBUTE_UNUSED,
                   ARMword pc ATTRIBUTE_UNUSED,
                   ARMword tinstr ATTRIBUTE_UNUSED,
                   ARMword * ainstr ATTRIBUTE_UNUSED)
{
    return t_undefined;
}

/* SWI handler. The only SWI we honour is the return sentinel, which stops
 * emulation cleanly; anything else is reported as unhandled so the caller sees
 * a fault rather than silent corruption. */
unsigned
ARMul_OSHandleSWI (ARMul_State * state, ARMword number)
{
    if (number == REPLAY_ARMSIM_SWI_RETURN)
    {
        if (state->OSptr != NULL)
            *(int *) state->OSptr = 1;
        state->Emulate = STOP;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                        */
/* ------------------------------------------------------------------ */

uint32_t
replay_armsim_return_opcode (void)
{
    /* Unconditional (AL) SWI carrying the sentinel comment field. */
    return 0xEF000000u | REPLAY_ARMSIM_SWI_RETURN;
}

ReplayArmSim *
replay_armsim_new (ReplayArmMode mode)
{
    ReplayArmSim *sim = calloc (1, sizeof (*sim));
    if (sim == NULL)
        return NULL;

    ARMul_EmulateInit ();  /* idempotent: fills the shared decode tables */

    sim->mode = mode;
    sim->returned = 0;
    sim->state = ARMul_NewState ();
    if (sim->state == NULL)
    {
        free (sim);
        return NULL;
    }

    /* MODE_32: 32-bit, ARMv4/StrongARM-class -- the broadest instruction set
     * among the target cores, so behaviour matches the Unicorn harness except
     * for the (now-correct) classic alignment semantics.
     * MODE_26: 26-bit ARM2/ARM3-class baseline. */
    if (mode == REPLAY_ARM_MODE_26)
        ARMul_SelectProcessor (sim->state, ARM_Fix26_Prop);
    else
        ARMul_SelectProcessor (sim->state, ARM_v4_Prop);

    if (!ARMul_MemoryInit (sim->state, 0))
    {
        free (sim->state);
        free (sim);
        return NULL;
    }

    /* Give the SWI handler a path back to our return flag. */
    sim->state->OSptr = (unsigned char *) &sim->returned;

    return sim;
}

void
replay_armsim_free (ReplayArmSim *sim)
{
    if (sim == NULL)
        return;
    if (sim->state != NULL)
    {
        ARMul_MemoryExit (sim->state);
        free (sim->state);
    }
    free (sim);
}

int
replay_armsim_map (ReplayArmSim *sim, uint32_t addr,
                   const void *data, size_t data_len, size_t region_len)
{
    if (sim == NULL || region_len < data_len)
        return -1;
    /* The address space is flat and zero-filled on demand, so a map is just
     * the initial write; the rest of the region reads back as zero. */
    if (data_len > 0)
        return replay_armsim_write (sim, addr, data, data_len);
    return 0;
}

int
replay_armsim_write (ReplayArmSim *sim, uint32_t addr,
                     const void *data, size_t len)
{
    const unsigned char *bytes = data;
    size_t i;
    if (sim == NULL || (data == NULL && len > 0))
        return -1;
    for (i = 0; i < len; i++)
        ARMul_SafeWriteByte (sim->state, (ARMword) (addr + i), bytes[i]);
    return 0;
}

int
replay_armsim_read (ReplayArmSim *sim, uint32_t addr, void *out, size_t len)
{
    unsigned char *bytes = out;
    size_t i;
    if (sim == NULL || (out == NULL && len > 0))
        return -1;
    for (i = 0; i < len; i++)
        bytes[i] = (unsigned char) ARMul_SafeReadByte (sim->state,
                                                       (ARMword) (addr + i));
    return 0;
}

void
replay_armsim_set_reg (ReplayArmSim *sim, unsigned reg, uint32_t value)
{
    if (sim == NULL || reg > 15)
        return;
    if (reg == 15)
        ARMul_SetPC (sim->state, value);
    else
        sim->state->Reg[reg] = value;
}

uint32_t
replay_armsim_get_reg (const ReplayArmSim *sim, unsigned reg)
{
    if (sim == NULL || reg > 15)
        return 0;
    if (reg == 15)
        return ARMul_GetPC (sim->state);
    return sim->state->Reg[reg];
}

ReplayArmStatus
replay_armsim_run (ReplayArmSim *sim, uint32_t start,
                   uint64_t max_instructions, uint64_t *executed)
{
    uint64_t budget = (max_instructions == 0) ? 2000000000ull : max_instructions;
    uint64_t count = 0;
    ReplayArmStatus status = REPLAY_ARM_LIMIT;

    if (sim == NULL)
        return REPLAY_ARM_FAULT;

    sim->returned = 0;
    sim->state->EndCondition = 0;
    ARMul_SetPC (sim->state, start);

    /* Single-step using the ARMulator's documented idiom (see sim_resume in
     * the GDB wrapper): feed the address returned by ARMul_DoInstr back into
     * r15 and re-prime the pipeline each step, otherwise the RESUME path skips
     * the prefetched instructions. */
    while (count < budget)
    {
        sim->state->Reg[15] = ARMul_DoInstr (sim->state);
        sim->state->NextInstr |= PRIMEPIPE;
        count++;

        if (sim->returned)
        {
            status = REPLAY_ARM_OK;
            break;
        }
        if (sim->state->EndCondition != 0)
        {
            status = REPLAY_ARM_FAULT;
            break;
        }
    }

    if (executed != NULL)
        *executed = count;
    return status;
}
