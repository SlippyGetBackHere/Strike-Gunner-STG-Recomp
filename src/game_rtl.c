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

#include "beam_frame_driver.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
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

  if (booting) {
    s_resume_pc = read_vector(0x00FFFCu);
  } else {
    ppu_checkOverscan(g_ppu);
    ppu_handleVblank(g_ppu);
  }
  snes_set_hdma_beam_enabled(g_snes, false);
  snes_set_raster_irq_beam_enabled(true);
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

const RtlGameInfo kGameInfo = {
    .title = "strikegunnerstg",
    .initialize = NULL,
    .run_frame = &GameRunOneFrame,
    .draw_ppu_frame = &GameDrawPpuFrame,
    .save_name_prefix = "save",
    .session_reset = &GameSessionReset,
};
