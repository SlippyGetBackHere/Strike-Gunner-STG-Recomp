/*
 * All weapons, every stage -- trusted plugin for the "All weapons every
 * stage" feature of the strikegunnerstg.gameplay package
 * (mods/preloaded/packages/).
 *
 * The game marks each special weapon as used once it is taken into a stage,
 * and the weapon select screen then shows it as NO USE for the rest of the
 * game. The marks are one byte per weapon, 15 weapons in screen order
 * (left to right, top to bottom): player 1 at $1BED-$1BFB, player 2 at
 * $1BFC-$1C0A. Only the weapon select code in bank $04 touches them: one
 * write per player when a weapon is confirmed ($04:82AD, $04:82E4) and the
 * reads that draw the marks, place the cursor and refuse a used weapon.
 *
 * Once a frame, after the guest's frame, both arrays are cleared, so every
 * weapon is always offered. Disabled, the plugin is never activated and the
 * frame callback is never registered: the stock game is unchanged.
 */
#include <stdbool.h>
#include <string.h>

#include "common_rtl.h"
#include "mod_runtime.h"

#define STG_USED_WEAPONS_P1 0x1BED
#define STG_USED_WEAPONS_P2 0x1BFC
#define STG_WEAPON_COUNT 15

static bool s_active;

static void AllWeaponsFrame(void)
{
  if (!s_active)
    return;
  memset(&g_ram[STG_USED_WEAPONS_P1], 0, STG_WEAPON_COUNT);
  memset(&g_ram[STG_USED_WEAPONS_P2], 0, STG_WEAPON_COUNT);
}

static void ActivateAllWeapons(void)
{
  s_active = true;
  snes_mod_register_frame_callback(AllWeaponsFrame);
}

static void ResetAllWeapons(void)
{
  s_active = false;
}

SNES_MOD_CONSTRUCTOR(RegisterStgWeaponMods)
{
  snes_mod_register_reset_callback(ResetAllWeapons);
  snes_mod_register_activation_plugin("strikegunnerstg.all_weapons",
                                      ActivateAllWeapons);
}
