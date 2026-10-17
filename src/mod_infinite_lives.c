/*
 * Infinite lives -- trusted plugin for the "Infinite lives" feature of the
 * strikegunnerstg.gameplay package (mods/preloaded/packages/).
 *
 * The game keeps each player's SPARE lives in WRAM: player 1 at $023A,
 * player 2 at $0244. On a death, $00:9514 (player 1) and its twin at
 * $00:95A0 (player 2) end the game if the count is zero and otherwise
 * decrement it -- the only decrements of either counter in the ROM
 * ($00:952E, $00:95BA). New games and continues load the count from the
 * option at $0238; an extra life increments it.
 *
 * Once a frame, after the guest's frame, a drop of exactly one is put back.
 * The counter therefore never reaches zero through deaths, so the game-over
 * branch is never taken, while every other write (new game, continue, extra
 * life) passes through untouched. Disabled, the plugin is never activated
 * and the frame callback is never registered: the stock game is unchanged.
 */
#include <stdbool.h>
#include <stdint.h>

#include "common_rtl.h"
#include "game_rtl.h"
#include "mod_runtime.h"

#define STG_LIVES_P1 0x023A
#define STG_LIVES_P2 0x0244

static const uint16_t kLivesAddr[2] = { STG_LIVES_P1, STG_LIVES_P2 };
static bool s_active;
static int s_prev[2] = { -1, -1 };

void StgInfiniteLivesForget(void)
{
  s_prev[0] = s_prev[1] = -1;
}

static void InfiniteLivesFrame(void)
{
  if (!s_active)
    return;
  for (int i = 0; i < 2; i++) {
    int now = g_ram[kLivesAddr[i]];
    if (s_prev[i] > 0 && now == s_prev[i] - 1) {
      g_ram[kLivesAddr[i]] = (uint8)s_prev[i];
      now = s_prev[i];
    }
    s_prev[i] = now;
  }
}

static void ActivateInfiniteLives(void)
{
  s_active = true;
  StgInfiniteLivesForget();
  snes_mod_register_frame_callback(InfiniteLivesFrame);
}

static void ResetInfiniteLives(void)
{
  s_active = false;
  StgInfiniteLivesForget();
}

SNES_MOD_CONSTRUCTOR(RegisterStgGameplayMods)
{
  snes_mod_register_reset_callback(ResetInfiniteLives);
  snes_mod_register_activation_plugin("strikegunnerstg.infinite_lives",
                                      ActivateInfiniteLives);
}
