/*
 * Custom music -- trusted plugin for the "Custom music" feature of the
 * strikegunnerstg.gameplay package (mods/preloaded/packages/).
 *
 * The game asks its sound driver for a song by writing the song number to
 * APU port 0 ($2140); sound effects go through port 1. The commands are fire
 * and forget: the game never waits for the driver to answer. Song numbers:
 *
 *   $04 title            $03 weapon select     $02 boss
 *   $01 stages 1, 3, 5   $07 stages 2, 4       $06 stage 6
 *   $08 stage 7          $09 stage 8           $0A stage clear (jingle)
 *   $05 game over / continue (jingle)          $14 stop
 *
 * Each song maps to a folder (a "slot") under the music folder. When the
 * game asks for a song whose slot has files, the command is swallowed, the
 * driver is told to stop instead, and one of the player's tracks is streamed
 * through the mod mixer. Stage songs use the current stage ($0230), so every
 * stage has its own slot even where the original shares a song. A slot with
 * no files of its own falls back to any/, and then to the original song.
 *
 * Port 0 also carries the sound driver upload at power-on and reset, a burst
 * of hundreds of writes; nothing is touched until that burst has been seen
 * and has finished.
 *
 * STG_MUSIC_LOG=<file> (or 1 for stderr) logs every song command and what
 * was done with it, plus once a second how many sound-chip voices are
 * sounding -- the check that a replaced song really is silent underneath.
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define STG_MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#define STG_MKDIR(p) mkdir((p), 0755)
#endif

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "game_rtl.h"
#include "mod_audio.h"
#include "mod_runtime.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/snes.h"

#define MUSIC_PACKAGE "strikegunnerstg.gameplay"
#define MUSIC_FEATURE "custom_music"
#define STG_STAGE 0x0230
#define SONG_STOP 0x14

enum {
  SLOT_TITLE, SLOT_WEAPON_SELECT,
  SLOT_STAGE1, SLOT_STAGE2, SLOT_STAGE3, SLOT_STAGE4,
  SLOT_STAGE5, SLOT_STAGE6, SLOT_STAGE7, SLOT_STAGE8,
  SLOT_BOSS, SLOT_STAGE_CLEAR, SLOT_GAME_OVER,
  SLOT_COUNT, SLOT_NONE = -1
};
static const char *const kSlotDir[SLOT_COUNT] = {
  "title", "weapon-select",
  "stage1", "stage2", "stage3", "stage4",
  "stage5", "stage6", "stage7", "stage8",
  "boss", "stage-clear", "game-over",
};

#define MAX_TRACKS 256
#define PATH_CAP 1024

/* ---- options -------------------------------------------------------- */

static bool s_active;
static bool s_random;
static bool s_loop = true;
static bool s_keep_jingles = true;
static bool s_match_loudness = true;
static int s_volume = 100;
static char s_root[PATH_CAP] = "music";

/* ---- the track being played (read by the audio thread) -------------- */

typedef struct Track {
  int16_t *pcm;
  uint32_t frames;
  uint32_t channels;
  uint32_t rate;
  uint32_t pos;
  bool loop;
} Track;
static Track s_track;

/* ---- driver/game state ---------------------------------------------- */

static int s_port0_this_frame;
static int s_quiet_frames;
static bool s_upload_seen;
static bool s_armed;
static int s_pending_slot = SLOT_NONE;
static int s_pending_stage;
static bool s_pending_stop;
static int s_playing_slot = SLOT_NONE;
static int s_playing_stage;
static char s_playing_dir[PATH_CAP];
static int s_next_index[SLOT_COUNT + 1];
static int s_last_index[SLOT_COUNT + 1];
static uint32_t s_frame;
static FILE *s_log;

static void MusicLog(const char *fmt, ...)
{
  if (!s_log)
    return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(s_log, "[music] f=%u ", s_frame);
  vfprintf(s_log, fmt, ap);
  fputc('\n', s_log);
  fflush(s_log);
  va_end(ap);
}

/* ---- WAV loading ------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int16_t ConvertSample(const uint8_t *p, int bits, bool is_float)
{
  if (is_float && bits == 32) {
    float f;
    memcpy(&f, p, 4);
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return (int16_t)(f * 32767.0f);
  }
  switch (bits) {
  case 8:  return (int16_t)((p[0] - 128) << 8);
  case 16: return (int16_t)rd16(p);
  case 24: return (int16_t)(p[1] | p[2] << 8);
  case 32: return (int16_t)(p[2] | p[3] << 8);
  default: return 0;
  }
}

/* Whole-file load into signed-16, at most stereo. Returns false and leaves
 * *out untouched on anything it cannot read. */
static bool LoadWav(const char *path, Track *out)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 44) { fclose(f); return false; }
  uint8_t *data = (uint8_t *)malloc((size_t)size);
  if (!data) { fclose(f); return false; }
  size_t got = fread(data, 1, (size_t)size, f);
  fclose(f);
  bool ok = false;
  if (got == (size_t)size && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4)) {
    int fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = NULL;
    uint32_t pcm_bytes = 0;
    for (size_t at = 12; at + 8 <= got;) {
      uint32_t len = rd32(data + at + 4);
      const uint8_t *body = data + at + 8;
      if (len > got - at - 8) len = (uint32_t)(got - at - 8);
      if (!memcmp(data + at, "fmt ", 4) && len >= 16) {
        fmt = rd16(body);
        channels = rd16(body + 2);
        rate = rd32(body + 4);
        bits = rd16(body + 14);
        if (fmt == 0xFFFE && len >= 26)
          fmt = rd16(body + 24);          /* WAVE_FORMAT_EXTENSIBLE subformat */
      } else if (!memcmp(data + at, "data", 4)) {
        pcm = body;
        pcm_bytes = len;
      }
      at += 8 + len + (len & 1);
    }
    const bool is_float = fmt == 3;
    const int bps = bits / 8;
    if (pcm && channels >= 1 && rate >= 8000 && rate <= 192000 && bps >= 1 &&
        (fmt == 1 || (is_float && bits == 32)) && bits % 8 == 0 && bits <= 32) {
      const uint32_t frames = pcm_bytes / (uint32_t)(bps * channels);
      const uint32_t out_ch = channels >= 2 ? 2 : 1;
      int16_t *samples = frames ? (int16_t *)malloc((size_t)frames * out_ch * sizeof(int16_t)) : NULL;
      if (samples) {
        for (uint32_t i = 0; i < frames; i++)
          for (uint32_t c = 0; c < out_ch; c++)
            samples[(size_t)i * out_ch + c] =
                ConvertSample(pcm + ((size_t)i * channels + c) * bps, bits, is_float);
        out->pcm = samples;
        out->frames = frames;
        out->channels = out_ch;
        out->rate = rate;
        out->pos = 0;
        ok = true;
      }
    }
  }
  free(data);
  return ok;
}

/* ---- loudness --------------------------------------------------------- */

/* The game's own music measures about -24 to -27 dBFS RMS (title, weapon
 * select and stage 1, peaks near -6 dBFS); a commercially mastered track is
 * typically -9 to -12, about 15 dB louder. Scale each track to a little
 * above the game's level (by ear, matched music sat slightly under the sound
 * effects) so the Volume option starts from a good balance. Never boost more
 * than 12 dB, and never so far that the peaks would clip. */
#define TARGET_RMS_DBFS (-21.0)

static void MatchLoudness(Track *t, const char *path)
{
  const size_t n = (size_t)t->frames * t->channels;
  if (!n)
    return;
  double sumsq = 0;
  int peak = 1;
  for (size_t i = 0; i < n; i++) {
    const int v = t->pcm[i];
    sumsq += (double)v * v;
    const int a = v < 0 ? -v : v;
    if (a > peak) peak = a;
  }
  const double rms = sqrt(sumsq / (double)n) / 32768.0;
  if (rms < 1e-6)
    return;
  double gain = pow(10.0, TARGET_RMS_DBFS / 20.0) / rms;
  if (gain > 4.0) gain = 4.0;
  if (gain * peak > 32000.0) gain = 32000.0 / peak;
  const int q = (int)(gain * 65536.0 + 0.5);
  for (size_t i = 0; i < n; i++) {
    int32_t v = (int32_t)(((int64_t)t->pcm[i] * q) >> 16);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    t->pcm[i] = (int16_t)v;
  }
  MusicLog("%s: %.1f dBFS RMS, scaled %+.1f dB", path, 20.0 * log10(rms),
           20.0 * log10(gain));
}

/* ---- streaming -------------------------------------------------------- */

/* Audio thread, APU lock held. */
static uint32_t TrackFill(void *user, int16_t *dst, uint32_t frames)
{
  Track *t = (Track *)user;
  uint32_t written = 0;
  while (written < frames) {
    if (t->pos >= t->frames) {
      if (!t->loop || t->frames == 0)
        break;
      t->pos = 0;
    }
    uint32_t n = t->frames - t->pos;
    if (n > frames - written)
      n = frames - written;
    memcpy(dst + (size_t)written * t->channels,
           t->pcm + (size_t)t->pos * t->channels,
           (size_t)n * t->channels * sizeof(int16_t));
    t->pos += n;
    written += n;
  }
  return written;
}

static void StopTrack(void)
{
  snes_mod_audio_stream_stop();      /* takes the APU lock: TrackFill is done */
  free(s_track.pcm);
  memset(&s_track, 0, sizeof(s_track));
}

/* ---- slots and playlists --------------------------------------------- */

static bool HasExt(const char *name, const char *ext)
{
  size_t n = strlen(name), e = strlen(ext);
  if (n <= e)
    return false;
  for (size_t i = 0; i < e; i++) {
    char a = name[n - e + i], b = ext[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (a != b)
      return false;
  }
  return true;
}

static int CmpNames(const void *a, const void *b)
{
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Fill names[] with the playable files in dir, sorted. Returns the count. */
static int ListTracks(const char *dir, char **names, int cap)
{
  int n = 0;
#if defined(_WIN32)
  char pattern[PATH_CAP];
  snprintf(pattern, sizeof(pattern), "%s/*", dir);
  struct _finddata_t fd;
  intptr_t h = _findfirst(pattern, &fd);
  if (h == -1)
    return 0;
  do {
    if (!(fd.attrib & _A_SUBDIR) && HasExt(fd.name, ".wav") && n < cap)
      names[n++] = strdup(fd.name);
  } while (_findnext(h, &fd) == 0);
  _findclose(h);
#else
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  for (struct dirent *e; (e = readdir(d)) != NULL;)
    if (e->d_name[0] != '.' && HasExt(e->d_name, ".wav") && n < cap)
      names[n++] = strdup(e->d_name);
  closedir(d);
#endif
  qsort(names, (size_t)n, sizeof(names[0]), CmpNames);
  return n;
}

static void FreeNames(char **names, int n)
{
  for (int i = 0; i < n; i++)
    free(names[i]);
}

static int PickIndex(int key, int count)
{
  if (count <= 1)
    return 0;
  if (s_random) {
    int pick;
    do pick = rand() % count; while (pick == s_last_index[key]);
    return pick;
  }
  return s_next_index[key] % count;
}

/* Start the next track for a slot. key indexes the playlist position (the
 * any/ fallback has its own, SLOT_COUNT). Returns false when nothing in dir
 * could be played. */
static bool StartFromDir(const char *dir, int key)
{
  char *names[MAX_TRACKS];
  int count = ListTracks(dir, names, MAX_TRACKS);
  bool started = false;
  for (int attempt = 0; attempt < count && !started; attempt++) {
    int idx = PickIndex(key, count);
    s_next_index[key] = idx + 1;
    s_last_index[key] = idx;
    char path[PATH_CAP];
    snprintf(path, sizeof(path), "%s/%s", dir, names[idx]);
    Track t = { 0 };
    if (!LoadWav(path, &t)) {
      MusicLog("cannot play %s (not a supported WAV)", path);
      continue;
    }
    if (s_match_loudness)
      MatchLoudness(&t, path);
    StopTrack();
    s_track = t;
    s_track.loop = s_loop;
    if (snes_mod_audio_stream_start(TrackFill, &s_track, s_track.rate,
                                    s_track.channels, s_volume)) {
      MusicLog("playing %s (%u Hz, %u ch, %.1f s)", path, s_track.rate,
               s_track.channels, s_track.rate ? (double)s_track.frames / s_track.rate : 0.0);
      snprintf(s_playing_dir, sizeof(s_playing_dir), "%s", dir);
      started = true;
    } else {
      StopTrack();
    }
  }
  FreeNames(names, count);
  return started;
}

static bool SlotDir(int slot, int stage, char *out, size_t cap)
{
  if (slot == SLOT_BOSS && stage >= 1 && stage <= 8) {
    snprintf(out, cap, "%s/boss-stage%d", s_root, stage);
    char *names[1];
    int n = ListTracks(out, names, 1);
    FreeNames(names, n);
    if (n)
      return true;
  }
  snprintf(out, cap, "%s/%s", s_root, kSlotDir[slot]);
  return true;
}

static bool SlotHasTracks(int slot, int stage)
{
  char dir[PATH_CAP], *names[1];
  SlotDir(slot, stage, dir, sizeof(dir));
  int n = ListTracks(dir, names, 1);
  FreeNames(names, n);
  if (n)
    return true;
  if (slot == SLOT_STAGE_CLEAR || slot == SLOT_GAME_OVER)
    return false;                       /* jingles never borrow from any/ */
  snprintf(dir, sizeof(dir), "%s/any", s_root);
  n = ListTracks(dir, names, 1);
  FreeNames(names, n);
  return n != 0;
}

static bool StartSlot(int slot, int stage)
{
  char dir[PATH_CAP];
  SlotDir(slot, stage, dir, sizeof(dir));
  if (StartFromDir(dir, slot))
    return true;
  if (slot == SLOT_STAGE_CLEAR || slot == SLOT_GAME_OVER)
    return false;
  snprintf(dir, sizeof(dir), "%s/any", s_root);
  return StartFromDir(dir, SLOT_COUNT);
}

static int SlotForSong(uint8_t song, int stage)
{
  switch (song) {
  case 0x04: return SLOT_TITLE;
  case 0x03: return SLOT_WEAPON_SELECT;
  case 0x02: return SLOT_BOSS;
  case 0x0A: return SLOT_STAGE_CLEAR;
  case 0x05: return SLOT_GAME_OVER;
  case 0x01: case 0x06: case 0x07: case 0x08: case 0x09:
    return stage >= 1 && stage <= 8 ? SLOT_STAGE1 + stage - 1 : SLOT_NONE;
  default:   return SLOT_NONE;
  }
}

/* ---- hooks ------------------------------------------------------------ */

/* Guest thread, from the register write path. Nonzero consumes the write. */
static int MusicApuWrite(uint16_t reg, uint8_t value)
{
  if (!s_active || reg != 0x2140)
    return 0;
  s_port0_this_frame++;
  if (!s_armed)
    return 0;
  const int stage = g_ram[STG_STAGE] + 1;
  const int slot = SlotForSong(value, stage);
  if (value == SONG_STOP || slot == SLOT_NONE) {
    MusicLog("cmd %02X stage %d: original (no slot)", value, stage);
    s_pending_stop = true;
    s_pending_slot = SLOT_NONE;
    return 0;
  }
  const bool jingle = slot == SLOT_STAGE_CLEAR || slot == SLOT_GAME_OVER;
  if ((jingle && s_keep_jingles) || !SlotHasTracks(slot, stage)) {
    MusicLog("cmd %02X stage %d: original (%s)", value, stage,
             jingle && s_keep_jingles ? "jingle kept" : "slot empty");
    s_pending_stop = true;
    s_pending_slot = SLOT_NONE;
    return 0;
  }
  MusicLog("cmd %02X stage %d: replaced (%s)", value, stage, kSlotDir[slot]);
  RtlApuWrite(0x2140, SONG_STOP);
  s_pending_slot = slot;
  s_pending_stage = stage;
  s_pending_stop = false;
  return 1;
}

static int SoundingVoices(void)
{
  if (!g_snes || !g_snes->apu || !g_snes->apu->dsp)
    return -1;
  int n = 0;
  for (int i = 0; i < 8; i++)
    if (g_snes->apu->dsp->channel[i].gain > 0x20)
      n++;
  return n;
}

static void MusicFrame(void)
{
  if (!s_active)
    return;
  s_frame++;
  /* The power-on upload is hundreds of port-0 writes a frame; arm once it
   * has been seen and the port has then been quiet for half a second. */
  if (s_port0_this_frame >= 8)
    s_upload_seen = true;
  s_quiet_frames = s_port0_this_frame ? 0 : s_quiet_frames + 1;
  s_port0_this_frame = 0;
  if (!s_armed && s_upload_seen && s_quiet_frames >= 30) {
    s_armed = true;
    MusicLog("armed (sound driver loaded)");
  }

  if (s_pending_stop) {
    s_pending_stop = false;
    if (s_playing_slot != SLOT_NONE)
      MusicLog("custom music stopped");
    StopTrack();
    s_playing_slot = SLOT_NONE;
  }
  if (s_pending_slot != SLOT_NONE) {
    const int slot = s_pending_slot, stage = s_pending_stage;
    s_pending_slot = SLOT_NONE;
    if (StartSlot(slot, stage)) {
      s_playing_slot = slot;
      s_playing_stage = stage;
    } else {
      /* The original was already swallowed; nothing playable after all. */
      MusicLog("slot %s had no playable file", kSlotDir[slot]);
      s_playing_slot = SLOT_NONE;
    }
  }
  if (s_playing_slot != SLOT_NONE && !snes_mod_audio_stream_active()) {
    /* Ended (next song), or stopped by the mixer's reset on a rewind: the
     * slot is still the right one, so play from it again. */
    if (!StartSlot(s_playing_slot, s_playing_stage))
      s_playing_slot = SLOT_NONE;
  }
  if (s_log && s_frame % 60 == 0)
    MusicLog("voices sounding: %d, custom: %s", SoundingVoices(),
             s_playing_slot != SLOT_NONE ? kSlotDir[s_playing_slot] : "none");
}

void StgCustomMusicOnReset(void)
{
  /* The machine reboots and uploads its sound driver again. */
  s_armed = false;
  s_upload_seen = false;
  s_quiet_frames = 0;
  s_pending_slot = SLOT_NONE;
  s_playing_slot = SLOT_NONE;
}

void StgCustomMusicOnStateLoaded(void)
{
  /* A loaded state carries a running sound driver. The mixer has already
   * stopped the stream; music resumes with the game's next song command. */
  if (!s_active)
    return;
  s_armed = true;
  s_pending_slot = SLOT_NONE;
  s_pending_stop = true;
}

/* ---- activation ------------------------------------------------------- */

static void ReadOption(const char *id, char *out, uint32_t cap, const char *fallback)
{
  if (!snes_mod_runtime_feature_option_value_c(MUSIC_PACKAGE, MUSIC_FEATURE, id, out, cap) || !out[0])
    snprintf(out, cap, "%s", fallback);
}

static void CreateFolders(void)
{
  char path[PATH_CAP];
  STG_MKDIR(s_root);
  snprintf(path, sizeof(path), "%s/any", s_root);
  STG_MKDIR(path);
  for (int i = 0; i < SLOT_COUNT; i++) {
    snprintf(path, sizeof(path), "%s/%s", s_root, kSlotDir[i]);
    STG_MKDIR(path);
  }
  snprintf(path, sizeof(path), "%s/README.txt", s_root);
  FILE *f = fopen(path, "r");
  if (f) { fclose(f); return; }
  f = fopen(path, "w");
  if (!f)
    return;
  fputs("Custom music for Strike Gunner S.T.G\n"
        "====================================\n\n"
        "Put your own music files in these folders. Each folder replaces one\n"
        "piece of the game's music:\n\n"
        "  title          title screen\n"
        "  weapon-select  weapon select screen\n"
        "  stage1..stage8 each stage\n"
        "  boss           boss fights (or boss-stage1..boss-stage8 for one stage)\n"
        "  stage-clear    stage clear jingle (used only when \"Keep original\n"
        "                 jingles\" is off)\n"
        "  game-over      game over / continue jingle (same)\n"
        "  any            used for any music folder above that is empty\n\n"
        "A folder with several files plays them in order or at random (see the\n"
        "Mods page). A folder with no files, and nothing in any/, keeps the\n"
        "game's original music.\n\n"
        "Supported for now: WAV files.\n", f);
  fclose(f);
}

static void ActivateCustomMusic(void)
{
  char value[PATH_CAP];
  ReadOption("order", value, sizeof(value), "sequential");
  s_random = strcmp(value, "random") == 0;
  ReadOption("on_end", value, sizeof(value), "loop");
  s_loop = strcmp(value, "next") != 0;
  ReadOption("volume", value, sizeof(value), "100");
  s_volume = atoi(value);
  ReadOption("keep_jingles", value, sizeof(value), "true");
  s_keep_jingles = strcmp(value, "false") != 0;
  ReadOption("match_loudness", value, sizeof(value), "true");
  s_match_loudness = strcmp(value, "false") != 0;
  if (!snes_mod_runtime_resource_path_c(MUSIC_PACKAGE, MUSIC_FEATURE, "music_folder",
                                        value, sizeof(value)) || !value[0])
    snprintf(value, sizeof(value), "music");
  snprintf(s_root, sizeof(s_root), "%s", value);

  const char *log = getenv("STG_MUSIC_LOG");
  if (log && log[0])
    s_log = strcmp(log, "1") == 0 ? stderr : fopen(log, "a");

  srand((unsigned)time(NULL));
  for (int i = 0; i <= SLOT_COUNT; i++)
    s_last_index[i] = -1;
  CreateFolders();
  s_active = true;
  snes_mod_register_frame_callback(MusicFrame);
  snes_mod_register_apu_write_callback(MusicApuWrite);
  MusicLog("active: folder %s, %s, %s, volume %d%%, jingles %s, loudness %s",
           s_root, s_random ? "random" : "sequential", s_loop ? "loop" : "next",
           s_volume, s_keep_jingles ? "kept" : "replaceable",
           s_match_loudness ? "matched" : "as recorded");
}

static void ResetCustomMusic(void)
{
  s_active = false;
}

SNES_MOD_CONSTRUCTOR(RegisterStgMusicMods)
{
  snes_mod_register_reset_callback(ResetCustomMusic);
  snes_mod_register_activation_plugin("strikegunnerstg.custom_music",
                                      ActivateCustomMusic);
}
