/*
 * Stage select -- trusted plugin for the "Stage select" feature of the
 * strikegunnerstg.gameplay package (mods/preloaded/packages/).
 *
 * After the special weapon is chosen, a list of the eight stages comes up
 * (the framework's choice menu: the game is frozen meanwhile) and the game
 * goes on to the stage picked. The stage the game would have played next is
 * highlighted, so confirming straight away changes nothing.
 *
 * How the game sequences stages ($00:8075..$00:82E3): before every stage it
 * runs the weapon select screen (JSL $04:81A7), then reads the stage number
 * at $0230 ($00:8088, 0-based) and loads that stage; clearing a stage
 * increments $0230 and loops back. So $0230 written any time before the
 * weapon select returns picks the stage.
 *
 * Detecting "the weapon was just chosen": weapon select clears each player's
 * weapon ($023D, $0247) when it opens and stores weapon+1 on confirm
 * ($04:8298, $04:82CF; player 2 only in a two-player game, $0234). The
 * attract demo also sets both weapons, without the screen, so the screen
 * itself is required: its countdown timer at $0547 runs only there, and the
 * demo runs with $17E6 set. After the confirm the screen still fades out
 * over several frames before returning, which is when the list appears.
 *
 * Stage 1 means a new game to the code after the screen: with $0230 = 0 it
 * resets lives and a few counters ($00:808D..$00:80B9). Picking a later stage
 * from there would skip that, so the mod does the same resets itself.
 * Picking stage 1 later in a game gets the game's own reset (the same as a
 * new game: lives back to the starting number).
 *
 * Disabled, the plugin is never activated: the stock game is unchanged.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common_rtl.h"
#include "game_rtl.h"
#include "mod_runtime.h"
#include "snes_choice_menu.h"

#define SS_PACKAGE "strikegunnerstg.gameplay"
#define SS_FEATURE "stage_select"

#define STG_STAGE        0x0230
#define STG_TWO_PLAYERS  0x0234
#define STG_DEMO         0x17E6
#define STG_WS_TIMER     0x0547   /* 16-bit, weapon select only */
#define STG_WEAPON_P1    0x023D
#define STG_WEAPON_P2    0x0247
#define STG_STAGE_COUNT  8

static bool s_active;
static bool s_every_stage = true;
static bool s_in_select;          /* the weapon select screen is up */
static bool s_offered;            /* the list was shown for this screen */
static bool s_new_game;           /* next pick follows a game start */
static int  s_prev_timer = -1;

void StgStageSelectForget(void)
{
  s_in_select = false;
  s_offered = false;
  s_prev_timer = -1;
}

static void ReadOption(const char *id, char *out, uint32_t cap, const char *fallback)
{
  if (!snes_mod_runtime_feature_option_value_c(SS_PACKAGE, SS_FEATURE, id, out, cap) || !out[0])
    snprintf(out, cap, "%s", fallback);
}

/* $00:808D..$00:80B9, which the game runs only when the stage is 0. */
static void NewGameResets(void)
{
  g_ram[0x023B] = 0;
  g_ram[0x0245] = 0;
  g_ram[0x023C] = 0;
  g_ram[0x0246] = 0;
  g_ram[0x0244] = 0;
  g_ram[0x023A] = g_ram[0x0238];
  if (g_ram[STG_TWO_PLAYERS])
    g_ram[0x0244] = g_ram[0x0238];
  if (g_ram[0x0233] < 2) {
    g_ram[0x023C] = 5;
    g_ram[0x0246] = 5;
  }
}

static void StagePicked(int index, void *user)
{
  (void)user;
  if (index < 0 || index >= STG_STAGE_COUNT)
    return;                         /* cancelled: the stage the game chose */
  const int stage = g_ram[STG_STAGE];
  if (index == stage)
    return;
  if (stage == 0)
    NewGameResets();
  g_ram[STG_STAGE] = (uint8)index;
  fprintf(stderr, "[stage_select] stage %d chosen (game had %d)\n", index + 1, stage + 1);
}

static void OfferStages(void)
{
  static const char *const kItems[STG_STAGE_COUNT] = {
    "STAGE 1", "STAGE 2", "STAGE 3", "STAGE 4",
    "STAGE 5", "STAGE 6", "STAGE 7", "STAGE 8",
  };
  int next = g_ram[STG_STAGE];
  if (next >= STG_STAGE_COUNT) next = 0;
  if (!snes_choice_menu_open("STAGE SELECT", kItems, STG_STAGE_COUNT, next, 1,
                             StagePicked, NULL))
    fprintf(stderr, "[stage_select] could not open the stage list\n");
}

static void StageSelectFrame(void)
{
  if (!s_active)
    return;
  const int timer = g_ram[STG_WS_TIMER] | g_ram[STG_WS_TIMER + 1] << 8;
  const bool timer_moved = s_prev_timer >= 0 && timer != s_prev_timer;
  s_prev_timer = timer;

  const bool demo = g_ram[STG_DEMO] != 0;
  const bool p1_chosen = g_ram[STG_WEAPON_P1] != 0;
  const bool both_chosen = p1_chosen &&
      (!g_ram[STG_TWO_PLAYERS] || g_ram[STG_WEAPON_P2] != 0);

  /* The screen is up while its timer runs with no weapon chosen yet. */
  if (!demo && timer_moved && !p1_chosen && !s_in_select) {
    s_in_select = true;
    s_offered = false;
    if (g_ram[STG_STAGE] == 0)
      s_new_game = true;
  }
  if (!s_in_select)
    return;
  if (demo) {
    StgStageSelectForget();
    return;
  }
  if (both_chosen && !s_offered) {
    s_offered = true;
    s_in_select = false;
    const bool offer = s_every_stage || s_new_game;
    s_new_game = false;
    if (offer)
      OfferStages();
  }
}

static void ActivateStageSelect(void)
{
  char value[32];
  ReadOption("when", value, sizeof(value), "every");
  s_every_stage = strcmp(value, "start") != 0;
  s_active = true;
  s_new_game = false;
  StgStageSelectForget();
  snes_mod_register_frame_callback(StageSelectFrame);
}

static void ResetStageSelect(void)
{
  s_active = false;
  StgStageSelectForget();
}

SNES_MOD_CONSTRUCTOR(RegisterStgStageSelect)
{
  snes_mod_register_reset_callback(ResetStageSelect);
  snes_mod_register_activation_plugin("strikegunnerstg.stage_select",
                                      ActivateStageSelect);
}
