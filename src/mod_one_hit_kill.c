/*
 * One-hit kills -- trusted plugin for the "One-hit kills" feature of the
 * strikegunnerstg.gameplay package (mods/preloaded/packages/).
 *
 * Every enemy goes down to the first hit that damages it. Enemy health is
 * the 16-bit word at offset 6 of the enemy's object slot (the most any enemy
 * starts with is 200); each enemy's own handler removes it once that is 0.
 * Three damage sources:
 *
 * - The main gun. A shot that touches an enemy ($00:E5E6..$00:E83E, also
 *   $00:EC92) takes the low byte of the entry for the player's shot power
 *   ($023C / $0246) in the table at $00:E599 off its health (10 entries; the
 *   high byte is the hit box). The mod sets every low byte to $FF, more than
 *   any health.
 *
 * - Special weapons. A special weapon that touches an enemy calls the hit
 *   routine at $00:EF34 with a damage kind; the damage is a 16-bit word from
 *   the table at $00:E579 (16 entries, indexed by kind). The mod sets every
 *   nonzero entry to $7FFF. Entries that are 0 (hits that do no damage) stay
 *   0.
 *
 * - Large enemies partly drawn into the background (the stage 4 carrier,
 *   some bosses) keep their health in $1ADE ($FFFF while there is none),
 *   and hits on the background part come off it through the same two tables
 *   ($00:D455, $00:EEE4, $03:F364). Their health can be over 255 (the
 *   carrier has 400), more than a main-gun hit can take. So once a frame,
 *   after the guest's frame, a drop in $1ADE of exactly the patched main-gun
 *   damage ($FF), not from $FFFF, is taken as a hit and finished: $1ADE is
 *   only the enemy's copy (it writes its own health, offset 6 of its slot,
 *   back there every frame), so the enemy slot holding the same health is
 *   zeroed too. An enemy writing its health there as it appears is not a
 *   hit. (A first version zeroed every drop, which killed each such enemy as
 *   it appeared.) Forgotten on state load, rewind and reset.
 *
 * The tables are ROM data, read at run time through the cartridge buffer
 * and only by those damage paths, so the change is made there; the original
 * bytes are checked before anything is changed (a mismatch leaves the ROM
 * alone) and put back when the feature is turned off. Disabled, the plugin
 * is never activated: the stock game is unchanged.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common_rtl.h"
#include "game_rtl.h"
#include "mod_runtime.h"
#include "snes/cart.h"
#include "snes/snes.h"

#define DAMAGE_TABLE_OFFSET 0x6579   /* $00:E579, LoROM */
#define DAMAGE_TABLE_BYTES  32
#define SHOT_TABLE_OFFSET   0x6599   /* $00:E599 */
#define SHOT_TABLE_BYTES    20
#define STG_BG_TARGET_HP    0x1ADE
#define STG_OBJECTS         0x056F   /* 128 slots of 32 bytes */
#define STG_SLOT_BYTES      32
#define STG_SLOT_COUNT      128
#define STG_FIRST_ENEMY_SLOT 28      /* below: players and their shots */

static const uint8_t kStockTable[DAMAGE_TABLE_BYTES] = {
  0x00, 0x00, 0x01, 0x00, 0x0a, 0x00, 0xc8, 0x00, 0x03, 0x00, 0x00, 0x10,
  0x01, 0x00, 0x0f, 0x00, 0x01, 0x00, 0x02, 0x00, 0x05, 0x00, 0x0a, 0x00,
  0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00,
};
static const uint8_t kStockShots[SHOT_TABLE_BYTES] = {
  0x01, 0x04, 0x01, 0x04, 0x02, 0x08, 0x02, 0x08, 0x03, 0x0c,
  0x03, 0x0c, 0x04, 0x08, 0x04, 0x08, 0x05, 0x0c, 0x05, 0x0c,
};

static bool s_active;
static bool s_patched;
static int s_prev_bg_hp = -1;

void StgOneHitKillForget(void)
{
  s_prev_bg_hp = -1;
}

static uint8_t *RomAt(uint32_t offset, uint32_t bytes)
{
  if (!g_snes || !g_snes->cart || !g_snes->cart->rom ||
      (uint32_t)g_snes->cart->romSize < offset + bytes)
    return NULL;
  return g_snes->cart->rom + offset;
}

static void PatchTables(void)
{
  uint8_t *t = RomAt(DAMAGE_TABLE_OFFSET, DAMAGE_TABLE_BYTES);
  uint8_t *s = RomAt(SHOT_TABLE_OFFSET, SHOT_TABLE_BYTES);
  if (!t || !s || s_patched)
    return;
  if (memcmp(t, kStockTable, DAMAGE_TABLE_BYTES) != 0 ||
      memcmp(s, kStockShots, SHOT_TABLE_BYTES) != 0) {
    fprintf(stderr, "[one_hit_kill] damage tables not as expected; left alone\n");
    return;
  }
  for (int i = 0; i < DAMAGE_TABLE_BYTES; i += 2) {
    if (t[i] | t[i + 1]) {
      t[i] = 0xff;
      t[i + 1] = 0x7f;
    }
  }
  for (int i = 0; i < SHOT_TABLE_BYTES; i += 2)
    s[i] = 0xff;                      /* damage; s[i + 1] is the hit box */
  s_patched = true;
}

static void RestoreTables(void)
{
  uint8_t *t = RomAt(DAMAGE_TABLE_OFFSET, DAMAGE_TABLE_BYTES);
  uint8_t *s = RomAt(SHOT_TABLE_OFFSET, SHOT_TABLE_BYTES);
  if (t && s && s_patched) {
    memcpy(t, kStockTable, DAMAGE_TABLE_BYTES);
    memcpy(s, kStockShots, SHOT_TABLE_BYTES);
  }
  s_patched = false;
}

static void OneHitKillFrame(void)
{
  if (!s_active)
    return;
  int hp = g_ram[STG_BG_TARGET_HP] | g_ram[STG_BG_TARGET_HP + 1] << 8;
  /* A hit takes exactly the patched main-gun damage off (a special weapon's
   * $7FFF already leaves 0). */
  if (s_prev_bg_hp >= 0 && s_prev_bg_hp != 0xFFFF && hp != 0 &&
      s_prev_bg_hp - hp == 0xFF) {
    /* $1ADE is the enemy's copy: its own health (offset 6 of its slot) was
     * reduced with it, and it writes that back every frame, so both go. */
    for (int slot = STG_FIRST_ENEMY_SLOT; slot < STG_SLOT_COUNT; slot++) {
      const uint32_t o = STG_OBJECTS + slot * STG_SLOT_BYTES;
      if (g_ram[o] != 0 && (g_ram[o + 6] | g_ram[o + 7] << 8) == hp) {
        g_ram[o + 6] = 0;
        g_ram[o + 7] = 0;
      }
    }
    g_ram[STG_BG_TARGET_HP] = 0;
    g_ram[STG_BG_TARGET_HP + 1] = 0;
    hp = 0;
  }
  s_prev_bg_hp = hp;
}

static void ActivateOneHitKill(void)
{
  s_active = true;
  StgOneHitKillForget();
  PatchTables();
  snes_mod_register_frame_callback(OneHitKillFrame);
}

static void ResetOneHitKill(void)
{
  s_active = false;
  StgOneHitKillForget();
  RestoreTables();
}

SNES_MOD_CONSTRUCTOR(RegisterStgOneHitKill)
{
  snes_mod_register_reset_callback(ResetOneHitKill);
  snes_mod_register_activation_plugin("strikegunnerstg.one_hit_kill",
                                      ActivateOneHitKill);
}
