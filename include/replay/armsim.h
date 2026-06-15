/* replay/armsim.h -- a small ARM execution sandbox.
 *
 * This wraps the vendored ARM Ltd / GDB "ARMulator" instruction emulator
 * (vendor/armulator) behind an opaque, dependency-free C API. Unlike the
 * Unicorn-based Python harnesses it replaces, the ARMulator models the classic
 * Acorn ARM (ARM2/ARM3/ARM6xx/ARM7xx/StrongARM) memory semantics directly:
 *
 *   - an unaligned LDR rotates the loaded word by the byte offset, and
 *   - an unaligned LDM/word load ignores the low two address bits.
 *
 * Those are exactly the behaviours Unicorn (modelling a much later core) gets
 * wrong, so the per-codec instruction-signature shims the Python harnesses
 * needed are unnecessary here.
 *
 * Execution stops when the emulated program returns to an address holding the
 * sentinel opcode from replay_armsim_return_opcode(); install that word at the
 * address you load into r14 before running.
 */

#ifndef REPLAY_ARMSIM_H
#define REPLAY_ARMSIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ReplayArmSim ReplayArmSim;

/* Instruction-set / mode the sandbox models.
 *
 * MODE_32 mirrors the Unicorn harness exactly (32-bit ARM, ARMv4/StrongARM
 * class) so results are byte-identical except where the classic alignment
 * semantics differ -- which is the whole point. MODE_26 models a 26-bit
 * ARM2/ARM3-class core; note its 64 MB address space means return/stack
 * sentinels must live below 0x04000000. */
typedef enum {
    REPLAY_ARM_MODE_32 = 0,
    REPLAY_ARM_MODE_26 = 1
} ReplayArmMode;

typedef enum {
    REPLAY_ARM_OK = 0,    /* returned to the sentinel cleanly */
    REPLAY_ARM_LIMIT,     /* hit the instruction budget */
    REPLAY_ARM_FAULT      /* abort/undefined/unexpected SWI with no handler */
} ReplayArmStatus;

/* Create/destroy a sandbox. Returns NULL on allocation failure. */
ReplayArmSim *replay_armsim_new(ReplayArmMode mode);
void replay_armsim_free(ReplayArmSim *sim);

/* The opcode to place at the return address. When the emulated program
 * branches there (via the r14 you set), execution stops with REPLAY_ARM_OK. */
uint32_t replay_armsim_return_opcode(void);

/* Map (or extend) a region covering [addr, addr + region_len). The first
 * data_len bytes are initialised from `data` (which may be NULL when data_len
 * is 0); the remainder reads back as zero. region_len must be >= data_len.
 * Mappings may be made in any order and may be sparse; the address space is a
 * flat 4 GB on demand. Returns 0 on success, -1 on error. */
int replay_armsim_map(ReplayArmSim *sim, uint32_t addr,
                      const void *data, size_t data_len, size_t region_len);

/* Bulk access to the simulated address space (byte-granular, any alignment).
 * Return 0 on success, -1 on error. */
int replay_armsim_write(ReplayArmSim *sim, uint32_t addr,
                        const void *data, size_t len);
int replay_armsim_read(ReplayArmSim *sim, uint32_t addr,
                       void *out, size_t len);

/* General-purpose registers r0..r15 (reg 0..15). r15 is the PC. */
void replay_armsim_set_reg(ReplayArmSim *sim, unsigned reg, uint32_t value);
uint32_t replay_armsim_get_reg(const ReplayArmSim *sim, unsigned reg);

/* Begin executing at `start`. Stops at the return sentinel (REPLAY_ARM_OK),
 * after `max_instructions` (REPLAY_ARM_LIMIT, 0 means a large default), or on
 * an unhandled fault (REPLAY_ARM_FAULT). The number of instructions actually
 * executed is stored in *executed when non-NULL. */
ReplayArmStatus replay_armsim_run(ReplayArmSim *sim, uint32_t start,
                                  uint64_t max_instructions,
                                  uint64_t *executed);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_ARMSIM_H */
