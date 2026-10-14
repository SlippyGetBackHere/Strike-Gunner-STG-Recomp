/*
 * Strike Gunner S.T.G — per-game runtime glue.
 *
 * THIS FILE IS THE PORT. Everything else the scaffold produced is layout,
 * build wiring, and packaging; this is where the actual work happens.
 *
 * The framework does not, and cannot, drive an arbitrary SNES game on its
 * own. Two responsibilities always land on the game host:
 *
 *   1. Deciding what "one frame" means for THIS title, and returning from
 *      run_frame() at that boundary. A real ROM's reset vector never
 *      returns — it enters a main loop that waits on vblank — so somebody
 *      has to choose the yield point.
 *   2. Delivering NMI/IRQ at the hardware edge (see
 *      snesrecomp/docs/FRAME_MODEL_HOSTS.md).
 *
 * This title cannot use the framework's beam-aligned driver
 * (runner/src/beam_frame_driver.c) for the CPU half as it stands. That driver
 * runs an NMI handler to its RTI as one host call. Strike Gunner's handler
 * ($00:86C4) is not that shape: it turns NMI off in NMITIMEN and then does
 * scene work that outlasts the field -- the demo-to-intro transition waits
 * whole frames inside it by polling HVBJOY ($05:A636), and leaving the demo
 * with Start runs a long setup loop at $05:B0F9. Run to RTI, both spin to
 * the interpreter's step cap and the handler is thrown away, which is the
 * freeze after the attract demo.
 *
 * So the field loop below is the framework's, with one change: NMI is taken
 * the way the CPU takes it. The interrupt frame is pushed, I is set, and the
 * instruction stream simply continues at the vector; the handler is ordinary
 * guest code from then on, yields at the field boundary like any other, and
 * its RTI returns into what it interrupted. Everything else -- the field cut
 * at the beam's V=225, parked time, raster IRQ delivery -- follows
 * beam_frame_driver.c, and drawing still goes through the framework.
 */

#include "game_rtl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "beam_frame_driver.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/saveload.h"
#include "snes/snes.h"

extern CpuState g_cpu;
extern Ppu *g_ppu;

/* One NTSC field: 262 scanlines x 1364 master clocks. */
#define STG_MASTER_CYCLES_PER_FIELD 357368ull
#define STG_MASTER_CYCLES_PER_LINE 1364u
#define STG_VBLANK_LINE 225u
#define STG_MAX_SLICES_PER_FIELD 1024

/* 0 until the first frame has booted from the reset vector. */
static uint32_t s_resume_pc;

/* The CPU executed WAI and is halted until an interrupt line asserts. */
static bool s_wai_halted;

/* End of the field being run; frame-wait stubs restore the deadline to it. */
static uint64_t s_frame_end;

uint64_t GameFrameDeadline(void) { return s_frame_end; }

/* Set by a stub that handed back a routine to run rather than to wait in. */
static bool s_resume_now;

void GameRequestImmediateResume(void) { s_resume_now = true; }

static uint32_t read_vector(uint32_t addr) {
  uint32_t lo = snes_read(g_snes, addr);
  uint32_t hi = snes_read(g_snes, addr + 1u);
  return (hi << 8) | lo;
}

static void update_resume_pc(void) {
  uint32_t resume = interp_bridge_lle_resume_pc();
  if (resume)
    s_resume_pc = resume;
}

/* Parked time: the beam and APU advance; the CPU does not. */
static void advance_parked(uint64_t until) {
  g_cpu.master_cycles = until;
  snes_refresh_exempt();
  snes_sync_master_clock(g_snes, g_cpu.master_cycles);
}

/* Vector the instruction stream to an interrupt handler without running it:
 * the main loop executes it, so it can span fields. */
static void take_interrupt(uint32_t vector) {
  cpu_push_interrupt_frame_at(&g_cpu, s_resume_pc);
  g_cpu._flag_I = 1;
  g_cpu._flag_D = 0;
  s_resume_pc = vector;
  s_wai_halted = false;
}

void GameRunOneFrame(void)
{
  const bool booting = s_resume_pc == 0;
  int slices = 0;
  uint64_t frame_end;

  {
    uint32_t to_vblank = snes_master_clocks_until_line(g_snes, STG_VBLANK_LINE);
    frame_end = g_snes->beamMasterLast +
                (to_vblank ? to_vblank : STG_MASTER_CYCLES_PER_FIELD);
  }
  s_frame_end = frame_end;

  s_resume_now = false;
  if (booting) {
    s_resume_pc = read_vector(0x00FFFCu);
  } else {
    ppu_checkOverscan(g_ppu);
    ppu_handleVblank(g_ppu);
  }
  snes_set_hdma_beam_enabled(g_snes, false);
  snes_set_raster_irq_beam_enabled(true);
  /* The beam follows the master clock here, so a $4212 read syncs the beam
   * to the CPU's time rather than adding the static-recomp polling tick on
   * top of time already charged. With the tick, compiled vblank waits ran
   * the beam ahead of the CPU and cut the next field short: the game's scene
   * logic ran at half rate, and busy stages slowed down far more than on the
   * console. */
  snes_set_beam_from_master_clock(true);
  ppu_rasterBegin(g_ppu);

  if (!booting && g_snes->nmiEnabled)
    take_interrupt(read_vector(0x00FFEAu));

  while (g_cpu.master_cycles < frame_end && slices++ < STG_MAX_SLICES_PER_FIELD) {
    uint64_t next;
    uint32_t to_irq;

    if (s_wai_halted && g_snes->inIrq)
      s_wai_halted = false;
    if (!s_wai_halted && !(g_snes->inIrq && !g_cpu._flag_I)) {
      interp_bridge_set_master_deadline(frame_end);
      (void)interp_bridge_run_until_quiescent(&g_cpu, s_resume_pc);
      interp_bridge_set_master_deadline(0);
      update_resume_pc();
      if (interp_bridge_lle_took_wai())
        s_wai_halted = true;
    }
    if (g_cpu.master_cycles >= frame_end)
      break;
    if (s_resume_now) {
      s_resume_now = false;
      continue;
    }
    if (g_snes->inIrq && !g_cpu._flag_I) {
      take_interrupt(read_vector(0x00FFEEu));
      continue;
    }
    /* Pay the beam what it owes first: those clocks can latch an IRQ. */
    if (g_cpu.master_cycles > g_snes->beamMasterLast) {
      snes_sync_master_clock(g_snes, g_cpu.master_cycles);
      continue;
    }
    /* Parked on a poll or halted on WAI: hardware time still passes. */
    to_irq = snes_master_clocks_until_irq(g_snes);
    if (to_irq)
      to_irq += 1;
    if (to_irq == 0 || to_irq > STG_MASTER_CYCLES_PER_LINE)
      to_irq = STG_MASTER_CYCLES_PER_LINE;
    next = g_cpu.master_cycles + to_irq;
    if (next > frame_end)
      next = frame_end;
    advance_parked(next);
  }

  /* Walk the beam to the boundary, not past it. */
  {
    int walk = 0;
    if (g_cpu.master_cycles < frame_end) /* slices exhausted */
      advance_parked(frame_end);
    while (g_snes->beamMasterLast < frame_end && walk++ < 1200) {
      uint64_t before = g_snes->beamMasterLast;
      uint64_t want = frame_end - before;
      snes_advance_master_cycles(g_snes, want > UINT32_MAX ? UINT32_MAX : (uint32_t)want);
      if (g_snes->beamMasterLast == before)
        break;
    }
  }
}

void GameDrawPpuFrame(void)
{
    snes_beam_frame_driver_draw_ppu_frame();
}

void GameSessionReset(void)
{
    /* Rematch / soft-return: clear anything that must not survive a new
     * session. recomp-ai-rules/NETPLAY.md §3 — sticky state that "has always
     * been fine" is the usual desync culprit, because single-player never
     * re-enters the boot path twice in one process. */
    s_resume_pc = 0;
    s_wai_halted = false;
}

/* ── Where the game is, across save states and resets ─────────────────────
 *
 * A save state holds the machine, not this driver's resume point, and the
 * 65816 has no PC register for the snapshot to capture: where the game will
 * continue lives in s_resume_pc. Without it a loaded state resumed at
 * whatever address the CPU had before the load, against restored RAM and
 * stack, and the game fell back to its title screen. The framework's
 * execution extension (common_rtl.h, RtlSaveExecutionState) carries the
 * bridge and beam residue the same trip. */

#define STG_STATE_MAGIC 0x53544731u /* 'STG1' */

typedef struct {
  uint32_t magic;
  uint32_t resume_pc;
  uint32_t wai_halted;
} StgExecState;

static StgExecState s_loaded;
static bool s_loaded_valid;

static void StgStateSaveExtra(SaveLoadInfo *sli)
{
  StgExecState st = { STG_STATE_MAGIC, s_resume_pc, s_wai_halted ? 1u : 0u };
  sli->func(sli, &st, sizeof(st));
  RtlSaveExecutionState(sli);
}

static void StgStateLoadExtra(SaveLoadInfo *sli, uint32_t version)
{
  (void)version;
  s_loaded_valid = false;
  if (RtlStateBytesRemaining(sli) < sizeof(s_loaded))
    return;
  sli->func(sli, &s_loaded, sizeof(s_loaded));
  if (s_loaded.magic != STG_STATE_MAGIC)
    return;
  s_loaded_valid = RtlLoadExecutionState(sli);
}

static void StgStateLoaded(uint32_t version)
{
  (void)version;
  /* The PPU's raster journal (ppu.c) holds the last field's register
   * baseline, which the renderer restores before drawing. It is not part of
   * the state, so after a load it still held whatever was on screen before
   * -- the mode 7 intro, in a fresh launch -- and every frame on which the
   * game did not rewrite BGMODE/TM itself drew from it: a garbled screen
   * over correct game state. Re-arm it from the restored registers. */
  ppu_rasterReset();
  if (!s_loaded_valid) {
    /* A state from a build that did not record the resume point: there is
     * nowhere correct to continue, so boot rather than run from a guess. */
    fprintf(stderr, "[stg] save state has no resume point; rebooting\n");
    s_resume_pc = 0;
    s_wai_halted = false;
    return;
  }
  RtlApplyExecutionState();
  s_resume_pc = s_loaded.resume_pc;
  s_wai_halted = s_loaded.wai_halted != 0;
  s_loaded_valid = false;
}

/* Rewind snapshots (in memory, this build only). */
static size_t StgExecStateBound(void) { return sizeof(StgExecState); }

static size_t StgExecStateSave(void *out, size_t capacity)
{
  StgExecState st = { STG_STATE_MAGIC, s_resume_pc, s_wai_halted ? 1u : 0u };
  if (capacity < sizeof(st))
    return 0;
  memcpy(out, &st, sizeof(st));
  return sizeof(st);
}

static int StgExecStateLoad(const void *in, size_t size)
{
  StgExecState st;
  if (size != sizeof(st))
    return 0;
  memcpy(&st, in, sizeof(st));
  if (st.magic != STG_STATE_MAGIC)
    return 0;
  s_resume_pc = st.resume_pc;
  s_wai_halted = st.wai_halted != 0;
  return 1;
}

/* The Reset hotkey: the machine reboots, so must the driver. */
static void StgHardwareReset(void)
{
  s_resume_pc = 0;
  s_wai_halted = false;
}

const RtlGameInfo kGameInfo = {
    .title = "strikegunnerstg",
    .initialize = NULL,
    .run_frame = &GameRunOneFrame,
    .draw_ppu_frame = &GameDrawPpuFrame,
    .save_name_prefix = "save",
    .state_save_extra = &StgStateSaveExtra,
    .state_load_extra = &StgStateLoadExtra,
    .on_state_loaded = &StgStateLoaded,
    .session_reset = &GameSessionReset,
    .hardware_reset = &StgHardwareReset,
    .exec_state_bound = &StgExecStateBound,
    .exec_state_save = &StgExecStateSave,
    .exec_state_load = &StgExecStateLoad,
};
