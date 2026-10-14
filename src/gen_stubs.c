/* Hand-written bodies for code the recompiler could not translate.
 *
 * Empty at scaffold time. Fill this in as regeneration reports unresolved
 * targets — and fill it in with real behaviour, never with a value invented
 * to get past the gap. An invented result turns a loud failure into a silent
 * wrong one (recomp-ai-rules/PRINCIPLES.md, "No stubs"). If a routine cannot
 * be translated yet, leave it to the interpreter tier instead.
 */

#include "cpu_state.h"
#include "game_rtl.h"
#include "snes/interp_bridge.h"

/* Frame waits.
 *
 * These routines do not compute anything: they spin until the next vblank.
 * Compiled code cannot host one, because the wait only ends when the frame
 * driver gets control back to advance the beam and take NMI. Each stub hands
 * the routine's real ROM entry back to the scheduler frame (recomp/bank*.cfg
 * `hle_func`; snesrecomp/docs/LLE_SCHEDULER.md), which interprets the original
 * bytes and yields at the poll like any other parked guest code. Nothing
 * about the wait is reimplemented here.
 *
 * The hand-back has to reach the frame driver, not merely the nearest
 * interpreter frame: a plain yield unwind is consumed by whichever bridge
 * frame bounced into compiled code, and when that is a nested gap run it
 * interprets the wait itself and spins to the step cap, since nothing above
 * it can advance the beam. So the stub yields the way a compiled block does
 * when the field's clock runs out -- pull the deadline in to now, let the
 * bridge see it reached, unwind -- which every nested frame passes through.
 *
 *   $00:83C9  PHP / SEP #$20 / LDA $0542 / BEQ * / STZ $0542 / PLP / RTL
 *             waits for the flag the NMI handler sets.
 *   $00:9144  STX $0545 / LDX $0545 / BNE * / RTL
 *             waits X frames; the NMI handler counts $0545 down.
 *   $00:9151  the same wait, ended early by a pad bit through ($90).
 *   $05:A636  polls HVBJOY ($4212) for the end, then the start, of vblank.
 */
static RecompReturn StgFrameWait(CpuState *cpu, uint32 entry_pc24,
                                 uint8 frame_size)
{
    if (interp_bridge_in_lle_scheduler()) {
        interp_bridge_set_master_deadline(cpu->master_cycles);
        if (!interp_bridge_lle_master_deadline_reached(cpu))
            interp_bridge_set_master_deadline(GameFrameDeadline());
        return interp_bridge_lle_yield_unwind(cpu, entry_pc24);
    }
    /* No scheduler frame beneath: run the original, balanced. */
    return interp_tier_run_call_frame(cpu, entry_pc24, entry_pc24, frame_size,
                                      NULL);
}

RecompReturn HleStgWaitNmiFlag(CpuState *cpu)
{
    return StgFrameWait(cpu, 0x0083C9u, 3);
}

RecompReturn HleStgWaitFrames(CpuState *cpu)
{
    return StgFrameWait(cpu, 0x009144u, 3);
}

RecompReturn HleStgWaitFramesOrInput(CpuState *cpu)
{
    return StgFrameWait(cpu, 0x009151u, 3);
}

RecompReturn HleStgWaitVblankPoll(CpuState *cpu)
{
    return StgFrameWait(cpu, 0x05A636u, 2);
}
