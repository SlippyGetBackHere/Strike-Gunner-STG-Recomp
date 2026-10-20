/*
 * Infinite special weapon -- trusted plugin for the "Infinite special
 * weapon" feature of the strikegunnerstg.gameplay package
 * (mods/preloaded/packages/).
 *
 * Each player's special weapon gauge (the bar in their side panel) is a
 * 16-bit value: player 1 at $023E, player 2 at $0248, full at $0300. Firing
 * the special weapon (A) takes a share of it ($30 for the first weapon).
 *
 * Once a frame, after the guest's frame, a gauge that went down since the
 * last frame is put back, so it never empties. A gauge that went up (the
 * game refilling it) is accepted. What counts as "the last frame" is
 * forgotten whenever the player's weapon or the stage changes, and on state
 * load, rewind and reset, so the mod never carries one weapon's or one
 * save's value into another. Disabled, the plugin is never activated: the
 * stock game is unchanged.
 */
#include <stdbool.h>
#include <stdint.h>

#include "common_rtl.h"
#include "game_rtl.h"
#include "mod_runtime.h"

#define STG_STAGE 0x0230

static const uint16_t kGaugeAddr[2]  = { 0x023E, 0x0248 };
static const uint16_t kWeaponAddr[2] = { 0x023D, 0x0247 };

static bool s_active;
static int s_prev[2] = { -1, -1 };
static int s_key[2] = { -1, -1 };      /* stage << 8 | weapon the value belongs to */

void StgInfiniteSpecialForget(void)
{
  s_prev[0] = s_prev[1] = -1;
  s_key[0] = s_key[1] = -1;
}

static void InfiniteSpecialFrame(void)
{
  if (!s_active)
    return;
  for (int i = 0; i < 2; i++) {
    const uint16_t a = kGaugeAddr[i];
    int now = g_ram[a] | g_ram[a + 1] << 8;
    const int key = g_ram[STG_STAGE] << 8 | g_ram[kWeaponAddr[i]];
    if (key != s_key[i]) {
      s_key[i] = key;
      s_prev[i] = now;
      continue;
    }
    if (s_prev[i] >= 0 && now < s_prev[i]) {
      g_ram[a] = (uint8)s_prev[i];
      g_ram[a + 1] = (uint8)(s_prev[i] >> 8);
      now = s_prev[i];
    }
    s_prev[i] = now;
  }
}

static void ActivateInfiniteSpecial(void)
{
  s_active = true;
  StgInfiniteSpecialForget();
  snes_mod_register_frame_callback(InfiniteSpecialFrame);
}

static void ResetInfiniteSpecial(void)
{
  s_active = false;
  StgInfiniteSpecialForget();
}

SNES_MOD_CONSTRUCTOR(RegisterStgSpecialMods)
{
  snes_mod_register_reset_callback(ResetInfiniteSpecial);
  snes_mod_register_activation_plugin("strikegunnerstg.infinite_special",
                                      ActivateInfiniteSpecial);
}
