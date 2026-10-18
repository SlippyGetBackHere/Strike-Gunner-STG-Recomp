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
 *   $01 in stage 8: the ending and credits (stage 8's own song is $09)
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
 * Tracks are WAV (with an optional smpl loop), MSU-1 .pcm (with its loop
 * point), or a pair song.intro.<ext> + song.loop.<ext> that loops the second
 * part only; MP3, OGG and FLAC when their decoders are built in. They load on
 * a worker thread so a long decode never stalls a frame, fade out briefly
 * when the music changes, and the slot playing is kept in save states.
 *
 * STG_MUSIC_LOG=<file> (or 1 for stderr) logs every song command and what
 * was done with it, plus once a second how many sound-chip voices are
 * sounding -- the check that a replaced song really is silent underneath.
 */
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#define STG_MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#define STG_MKDIR(p) mkdir((p), 0755)
#endif

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "desktop/sdl_compat.h"
#include "game_rtl.h"
#include "mod_audio.h"
#include "mod_runtime.h"
#include "music_decoders.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/saveload.h"
#include "snes/snes.h"
#include "snes_osd.h"

extern void RtlApuLock(void);
extern void RtlApuUnlock(void);

#define MUSIC_PACKAGE "strikegunnerstg.gameplay"
#define MUSIC_FEATURE "custom_music"
#define STG_STAGE 0x0230
#define SONG_STOP 0x14
#define FADE_SECONDS 0.35
#define CAPTION_MS 4000
#define HUD_RIGHT_MARGIN 18   /* the game's right-hand side panel, 1x pixels */

enum {
  SLOT_TITLE, SLOT_WEAPON_SELECT,
  SLOT_STAGE1, SLOT_STAGE2, SLOT_STAGE3, SLOT_STAGE4,
  SLOT_STAGE5, SLOT_STAGE6, SLOT_STAGE7, SLOT_STAGE8,
  SLOT_BOSS, SLOT_STAGE_CLEAR, SLOT_GAME_OVER, SLOT_ENDING,
  SLOT_COUNT, SLOT_NONE = -1
};
static const char *const kSlotDir[SLOT_COUNT] = {
  "title", "weapon-select",
  "stage1", "stage2", "stage3", "stage4",
  "stage5", "stage6", "stage7", "stage8",
  "boss", "stage-clear", "game-over", "ending",
};

#define MAX_TRACKS 256
#define PATH_CAP 1024

/* ---- options ---------------------------------------------------------- */

static bool s_active;
static bool s_random;
static bool s_loop = true;
static bool s_keep_jingles = true;
static bool s_match_loudness = true;
static bool s_caption_on = true;
static int s_caption_anchor = SNES_OSD_CAPTION_BOTTOM_CENTER;
static int s_skip_scancode = -1;
static int s_volume = 100;
static char s_root[PATH_CAP] = "music";

/* ---- tracks ----------------------------------------------------------- */

typedef struct Track {
  int16_t *pcm;
  uint32_t frames;
  uint32_t channels;
  uint32_t rate;
  uint32_t loop_start;   /* where a loop restarts (0, or the end of an intro) */
  uint32_t pos;
  bool loop;
  uint32_t fade_total;   /* fading out over fade_total frames when nonzero */
  uint32_t fade_left;
  bool finished;         /* ran out (not looping) */
  bool faded;            /* fade-out completed */
  char title[160];
  char path[PATH_CAP];   /* the file (the loop half of a pair), for save states */
} Track;

/* The track the audio thread reads while the stream is active. Only the
 * game thread changes it, and only while the stream is stopped (the stream
 * calls take the APU lock), except the fade fields, set under the lock. */
static Track s_track;
static bool s_have_track;

/* ---- what should play / what is playing -------------------------------- */

static int s_want_slot = SLOT_NONE, s_want_stage;
static int s_cur_slot = SLOT_NONE, s_cur_stage;
static bool s_skip_pending;
static int s_next_index[SLOT_COUNT + 1];
static int s_last_index[SLOT_COUNT + 1];

/* ---- driver/game state ------------------------------------------------- */

static int s_port0_this_frame;
static int s_quiet_frames;
static bool s_upload_seen;
static bool s_armed;
static uint32_t s_frame;
static FILE *s_log;
static bool s_skip_key_was_down;

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

/* ---- names ------------------------------------------------------------ */

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

static bool IsPlayableName(const char *name)
{
  if (strstr(name, ".intro."))
    return false;                  /* the first half of an intro+loop pair */
  return HasExt(name, ".wav") || HasExt(name, ".pcm") ||
         (MusicDecoderMp3Available() && HasExt(name, ".mp3")) ||
         (MusicDecoderOggAvailable() && HasExt(name, ".ogg")) ||
         (MusicDecoderFlacAvailable() && HasExt(name, ".flac"));
}

/* "dir/My Song.loop.ogg" -> "My Song". A trailing "[...]" tag, such as the
 * video id a download tool appends ("My Song [dQw4w9WgXcQ]"), is dropped
 * too: it is not part of the name and takes room on screen. */
static void TitleFromPath(const char *path, char *out, size_t cap)
{
  const char *base = path;
  for (const char *p = path; *p; p++)
    if (*p == '/' || *p == '\\')
      base = p + 1;
  snprintf(out, cap, "%s", base);
  char *dot = strrchr(out, '.');
  if (dot)
    *dot = '\0';
  size_t n = strlen(out);
  if (n > 5 && !strcmp(out + n - 5, ".loop"))
    out[n - 5] = '\0';
  n = strlen(out);
  char *tag = strrchr(out, '[');
  if (n && out[n - 1] == ']' && tag && tag > out) {
    while (tag > out && tag[-1] == ' ')
      tag--;
    if (tag > out)
      *tag = '\0';
  }
}

/* ---- decoding --------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint8_t *ReadWholeFile(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = len > 0 ? (uint8_t *)malloc((size_t)len) : NULL;
  if (data && fread(data, 1, (size_t)len, f) != (size_t)len) {
    free(data);
    data = NULL;
  }
  fclose(f);
  *size = data ? (size_t)len : 0;
  return data;
}

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

static bool DecodeWav(const uint8_t *data, size_t got, Track *out)
{
  if (got < 44 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4))
    return false;
  int fmt = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t *pcm = NULL;
  uint32_t pcm_bytes = 0;
  int64_t loop_start = -1, loop_end = -1;
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
        fmt = rd16(body + 24);        /* WAVE_FORMAT_EXTENSIBLE subformat */
    } else if (!memcmp(data + at, "data", 4)) {
      pcm = body;
      pcm_bytes = len;
    } else if (!memcmp(data + at, "smpl", 4) && len >= 60 && rd32(body + 28) > 0) {
      loop_start = rd32(body + 44);   /* first loop: start, end (inclusive) */
      loop_end = rd32(body + 48);
    }
    at += 8 + len + (len & 1);
  }
  const bool is_float = fmt == 3;
  const int bps = bits / 8;
  if (!pcm || channels < 1 || rate < 8000 || rate > 192000 || bps < 1 ||
      !(fmt == 1 || (is_float && bits == 32)) || bits % 8 || bits > 32)
    return false;
  uint32_t frames = pcm_bytes / (uint32_t)(bps * channels);
  if (loop_end >= 0 && loop_end + 1 < frames)
    frames = (uint32_t)loop_end + 1;
  const uint32_t out_ch = channels >= 2 ? 2 : 1;
  int16_t *samples = frames ? (int16_t *)malloc((size_t)frames * out_ch * sizeof(int16_t)) : NULL;
  if (!samples)
    return false;
  for (uint32_t i = 0; i < frames; i++)
    for (uint32_t c = 0; c < out_ch; c++)
      samples[(size_t)i * out_ch + c] =
          ConvertSample(pcm + ((size_t)i * channels + c) * bps, bits, is_float);
  out->pcm = samples;
  out->frames = frames;
  out->channels = out_ch;
  out->rate = rate;
  out->loop_start = loop_start >= 0 && loop_start < frames ? (uint32_t)loop_start : 0;
  return true;
}

/* MSU-1 audio track: "MSU1", a 32-bit loop point in frames, then 44.1 kHz
 * signed-16 stereo. */
static bool DecodeMsuPcm(const uint8_t *data, size_t got, Track *out)
{
  if (got < 12 || memcmp(data, "MSU1", 4))
    return false;
  const uint32_t frames = (uint32_t)((got - 8) / 4);
  int16_t *samples = (int16_t *)malloc((size_t)frames * 4);
  if (!samples)
    return false;
  for (uint32_t i = 0; i < frames * 2; i++)
    samples[i] = (int16_t)rd16(data + 8 + (size_t)i * 2);
  const uint32_t loop = rd32(data + 4);
  out->pcm = samples;
  out->frames = frames;
  out->channels = 2;
  out->rate = 44100;
  out->loop_start = loop < frames ? loop : 0;
  return true;
}

static bool DecodeFile(const char *path, Track *out)
{
  memset(out, 0, sizeof(*out));
  if (HasExt(path, ".mp3") || HasExt(path, ".ogg") || HasExt(path, ".flac")) {
    MusicDecodedAudio a;
    if (!MusicDecodeCompressed(path, &a))
      return false;
    out->pcm = a.samples;
    out->frames = a.frames;
    out->channels = a.channels;
    out->rate = a.sample_rate;
    return true;
  }
  size_t size = 0;
  uint8_t *data = ReadWholeFile(path, &size);
  if (!data)
    return false;
  bool ok = HasExt(path, ".pcm") ? DecodeMsuPcm(data, size, out) : DecodeWav(data, size, out);
  free(data);
  return ok;
}

/* Join an intro and its loop into one track that loops from the seam. Both
 * must share a rate and channel count. */
static bool JoinIntro(Track *intro, Track *loop, Track *out)
{
  if (intro->rate != loop->rate || intro->channels != loop->channels)
    return false;
  const uint32_t ch = intro->channels;
  const uint32_t frames = intro->frames + loop->frames;
  int16_t *pcm = (int16_t *)malloc((size_t)frames * ch * sizeof(int16_t));
  if (!pcm)
    return false;
  memcpy(pcm, intro->pcm, (size_t)intro->frames * ch * sizeof(int16_t));
  memcpy(pcm + (size_t)intro->frames * ch, loop->pcm, (size_t)loop->frames * ch * sizeof(int16_t));
  *out = *loop;
  out->pcm = pcm;
  out->frames = frames;
  out->loop_start = intro->frames + loop->loop_start;
  return true;
}

/* ---- loudness --------------------------------------------------------- */

/* The game's own music measures about -24 to -27 dBFS RMS (title, weapon
 * select and stage 1, peaks near -6 dBFS); a commercially mastered track is
 * typically -9 to -12, about 15 dB louder. Scale each track to a little
 * above the game's level (by ear, matched music sat slightly under the sound
 * effects) so the Volume option starts from a good balance. Never boost more
 * than 12 dB, and never so far that the peaks would clip. */
#define TARGET_RMS_DBFS (-21.0)

static double MatchLoudness(Track *t)
{
  const size_t n = (size_t)t->frames * t->channels;
  if (!n)
    return 0;
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
    return 0;
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
  return 20.0 * log10(gain);
}

/* ---- the worker thread -------------------------------------------------- */

typedef struct LoadJob {
  char path[PATH_CAP];
  char intro[PATH_CAP];     /* empty unless an intro+loop pair */
  int slot, stage;
  uint32_t generation;
  bool match_loudness;
  bool ok;
  double gain_db;
  uint32_t decode_ms;       /* worker time, for the log */
  Track result;
} LoadJob;

static LoadJob s_job;
static atomic_int s_job_state;   /* 0 idle, 1 loading, 2 done */
static uint32_t s_job_generation;

static void RunJob(LoadJob *job)
{
  const uint64_t t0 = SDL_GetTicks();
  Track t;
  job->ok = DecodeFile(job->path, &t);
  if (job->ok && job->intro[0]) {
    Track intro, joined;
    if (DecodeFile(job->intro, &intro) && JoinIntro(&intro, &t, &joined)) {
      free(t.pcm);
      t = joined;
    }
    free(intro.pcm);
  }
  if (job->ok && job->match_loudness)
    job->gain_db = MatchLoudness(&t);
  if (job->ok) {
    TitleFromPath(job->path, t.title, sizeof(t.title));
    snprintf(t.path, sizeof(t.path), "%s", job->path);
    job->result = t;
  }
  job->decode_ms = (uint32_t)(SDL_GetTicks() - t0);
  atomic_store(&s_job_state, 2);
}

#if defined(_WIN32)
static DWORD WINAPI JobThread(LPVOID arg) { RunJob((LoadJob *)arg); return 0; }
static void StartJobThread(void)
{
  /* Below the game and audio threads: a decode burst must never take their
   * time (a 4-minute MP3 is ~150 ms of one core). */
  HANDLE h = CreateThread(NULL, 0, JobThread, &s_job, CREATE_SUSPENDED, NULL);
  if (!h) { RunJob(&s_job); return; }
  SetThreadPriority(h, THREAD_PRIORITY_LOWEST);
  ResumeThread(h);
  CloseHandle(h);
}
#else
static void *JobThread(void *arg) { RunJob((LoadJob *)arg); return NULL; }
static void StartJobThread(void)
{
  pthread_t t;
  if (pthread_create(&t, NULL, JobThread, &s_job) == 0) pthread_detach(t);
  else RunJob(&s_job);
}
#endif

/* ---- playlists ---------------------------------------------------------- */

static int CmpNames(const void *a, const void *b)
{
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

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
    if (!(fd.attrib & _A_SUBDIR) && IsPlayableName(fd.name) && n < cap)
      names[n++] = strdup(fd.name);
  } while (_findnext(h, &fd) == 0);
  _findclose(h);
#else
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  for (struct dirent *e; (e = readdir(d)) != NULL;)
    if (e->d_name[0] != '.' && IsPlayableName(e->d_name) && n < cap)
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

static int CountTracks(const char *dir)
{
  char *names[MAX_TRACKS];
  int n = ListTracks(dir, names, MAX_TRACKS);
  FreeNames(names, n);
  return n;
}

static void SlotDir(int slot, int stage, char *out, size_t cap)
{
  if (slot == SLOT_BOSS && stage >= 1 && stage <= 8) {
    snprintf(out, cap, "%s/boss-stage%d", s_root, stage);
    if (CountTracks(out))
      return;
  }
  snprintf(out, cap, "%s/%s", s_root, kSlotDir[slot]);
}

static bool IsJingle(int slot)
{
  return slot == SLOT_STAGE_CLEAR || slot == SLOT_GAME_OVER;
}

/* The folder a slot plays from right now (its own, or any/), with the
 * playlist key that goes with it. False when nothing is playable. */
static bool ResolveDir(int slot, int stage, char *dir, size_t cap, int *key)
{
  SlotDir(slot, stage, dir, cap);
  if (CountTracks(dir)) {
    *key = slot;
    return true;
  }
  if (IsJingle(slot))
    return false;                       /* jingles never borrow from any/ */
  snprintf(dir, cap, "%s/any", s_root);
  *key = SLOT_COUNT;
  return CountTracks(dir) != 0;
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

/* "song.loop.ogg" in dir -> "song.intro.<any playable ext>", if present. */
static void FindIntro(const char *dir, const char *name, char *out, size_t cap)
{
  out[0] = '\0';
  const char *loop = strstr(name, ".loop.");
  if (!loop)
    return;
  static const char *const kExt[] = { "wav", "pcm", "mp3", "ogg", "flac" };
  for (size_t i = 0; i < sizeof(kExt) / sizeof(kExt[0]); i++) {
    snprintf(out, cap, "%s/%.*s.intro.%s", dir, (int)(loop - name), name, kExt[i]);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return; }
  }
  out[0] = '\0';
}

/* A save state's song to load and the position to resume it at. */
static bool s_seek_pending;
static char s_seek_path[PATH_CAP];
static uint32_t s_seek_pos;

static void QueueJob(const char *dir, const char *name)
{
  memset(&s_job, 0, sizeof(s_job));
  snprintf(s_job.path, sizeof(s_job.path), "%s/%s", dir, name);
  FindIntro(dir, name, s_job.intro, sizeof(s_job.intro));
  s_job.slot = s_cur_slot;
  s_job.stage = s_cur_stage;
  s_job.generation = ++s_job_generation;
  s_job.match_loudness = s_match_loudness;
  atomic_store(&s_job_state, 1);
  MusicLog("loading %s", s_job.path);
  StartJobThread();
}

/* Queue the next track for the current slot on the worker. */
static void RequestLoad(void)
{
  if (atomic_load(&s_job_state) != 0 || s_cur_slot == SLOT_NONE)
    return;
  if (s_seek_pending) {
    /* The exact song a save state was playing, if it is still there. */
    FILE *f = fopen(s_seek_path, "rb");
    if (f) {
      fclose(f);
      char dir[PATH_CAP];
      snprintf(dir, sizeof(dir), "%s", s_seek_path);
      char *slash = strrchr(dir, '/');
      if (slash) {
        *slash = '\0';
        QueueJob(dir, slash + 1);
        return;
      }
    }
    s_seek_pending = false;                /* gone: play the slot as usual */
  }
  char dir[PATH_CAP];
  int key;
  if (!ResolveDir(s_cur_slot, s_cur_stage, dir, sizeof(dir), &key)) {
    MusicLog("slot %s has nothing playable now; silence", kSlotDir[s_cur_slot]);
    s_cur_slot = s_want_slot = SLOT_NONE;
    return;
  }
  char *names[MAX_TRACKS];
  const int count = ListTracks(dir, names, MAX_TRACKS);
  const int idx = PickIndex(key, count);
  s_next_index[key] = idx + 1;
  s_last_index[key] = idx;
  QueueJob(dir, names[idx]);
  FreeNames(names, count);
}

/* ---- streaming ---------------------------------------------------------- */

/* Audio thread, APU lock held. */
static uint32_t TrackFill(void *user, int16_t *dst, uint32_t frames)
{
  Track *t = (Track *)user;
  uint32_t written = 0;
  while (written < frames) {
    if (t->pos >= t->frames) {
      if (!t->loop || t->frames == 0 || t->loop_start >= t->frames) {
        t->finished = true;
        break;
      }
      t->pos = t->loop_start;
    }
    uint32_t n = t->frames - t->pos;
    if (n > frames - written)
      n = frames - written;
    int16_t *out = dst + (size_t)written * t->channels;
    memcpy(out, t->pcm + (size_t)t->pos * t->channels,
           (size_t)n * t->channels * sizeof(int16_t));
    if (t->fade_total) {
      if (n > t->fade_left)
        n = t->fade_left;
      for (uint32_t i = 0; i < n; i++) {
        const int64_t g = (int64_t)(t->fade_left - i) * 65536 / t->fade_total;
        for (uint32_t c = 0; c < t->channels; c++)
          out[i * t->channels + c] = (int16_t)((out[i * t->channels + c] * g) >> 16);
      }
      t->fade_left -= n;
      if (t->fade_left == 0) {
        t->faded = true;
        t->pos += n;
        written += n;
        break;
      }
    }
    t->pos += n;
    written += n;
  }
  return written;
}

static void FreeTrack(void)
{
  snes_mod_audio_stream_stop();      /* takes the APU lock: TrackFill is done */
  free(s_track.pcm);
  memset(&s_track, 0, sizeof(s_track));
  s_have_track = false;
}

static void BeginFade(void)
{
  RtlApuLock();
  if (!s_track.fade_total) {
    s_track.fade_total = (uint32_t)(s_track.rate * FADE_SECONDS);
    if (!s_track.fade_total) s_track.fade_total = 1;
    s_track.fade_left = s_track.fade_total;
  }
  RtlApuUnlock();
}

static bool StartStream(void)
{
  return snes_mod_audio_stream_start(TrackFill, &s_track, s_track.rate,
                                     s_track.channels, s_volume) != 0;
}

/* ---- song commands ------------------------------------------------------- */

static int SlotForSong(uint8_t song, int stage)
{
  switch (song) {
  case 0x04: return SLOT_TITLE;
  case 0x03: return SLOT_WEAPON_SELECT;
  case 0x02: return SLOT_BOSS;
  case 0x0A: return SLOT_STAGE_CLEAR;
  case 0x05: return SLOT_GAME_OVER;
  case 0x01:
    /* The ending and credits reuse stage 1's song; stage 8's own is $09, so
     * a $01 while the stage is 8 can only be the ending. */
    if (stage == 8)
      return SLOT_ENDING;
    /* fall through */
  case 0x06: case 0x07: case 0x08: case 0x09:
    return stage >= 1 && stage <= 8 ? SLOT_STAGE1 + stage - 1 : SLOT_NONE;
  default:   return SLOT_NONE;
  }
}

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
  char dir[PATH_CAP];
  int key;
  const char *why = NULL;
  if (value == SONG_STOP || slot == SLOT_NONE)
    why = "no slot";
  else if (IsJingle(slot) && s_keep_jingles)
    why = "jingle kept";
  else if (!ResolveDir(slot, stage, dir, sizeof(dir), &key))
    why = "slot empty";
  if (why) {
    MusicLog("cmd %02X stage %d: original (%s)", value, stage, why);
    s_want_slot = SLOT_NONE;
    return 0;
  }
  MusicLog("cmd %02X stage %d: replaced (%s)", value, stage, kSlotDir[slot]);
  RtlApuWrite(0x2140, SONG_STOP);
  s_want_slot = slot;
  s_want_stage = stage;
  if (slot == s_cur_slot && stage == s_cur_stage)
    s_skip_pending = true;          /* the same music asked for again: restart */
  return 1;
}

/* ---- the frame step ------------------------------------------------------ */

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

static bool SkipKeyPressed(void)
{
  if (s_skip_scancode < 0)
    return false;
  const void *state = SDL_GetKeyboardState(NULL);
  if (!state)
    return false;
#if SNESRECOMP_SDL3
  const bool down = ((const bool *)state)[s_skip_scancode];
#else
  const bool down = ((const Uint8 *)state)[s_skip_scancode] != 0;
#endif
  const bool pressed = down && !s_skip_key_was_down;
  s_skip_key_was_down = down;
  return pressed;
}

static void InstallLoadedTrack(void)
{
  LoadJob *job = &s_job;
  const bool current = job->ok && job->generation == s_job_generation &&
                       job->slot == s_cur_slot && job->stage == s_cur_stage &&
                       !s_have_track;
  if (!current) {
    if (job->ok)
      free(job->result.pcm);
    if (!job->ok)
      MusicLog("cannot play %s", job->path);
    atomic_store(&s_job_state, 0);
    if (!job->ok && s_cur_slot != SLOT_NONE) {
      /* A bad file: move past it; give up on the slot after a full lap. */
      static int failures;
      if (++failures > MAX_TRACKS) {
        failures = 0;
        s_cur_slot = s_want_slot = SLOT_NONE;
      }
    }
    return;
  }
  s_track = job->result;
  s_track.loop = s_loop;
  s_have_track = true;
  atomic_store(&s_job_state, 0);
  if (s_seek_pending && !strcmp(job->path, s_seek_path)) {
    s_track.pos = s_seek_pos < s_track.frames ? s_seek_pos : 0;
    s_seek_pending = false;
    MusicLog("resuming at %.1f s", s_track.rate ? (double)s_track.pos / s_track.rate : 0.0);
  }
  if (!StartStream()) {
    FreeTrack();
    return;
  }
  MusicLog("playing %s%s%s (%u Hz, %u ch, %.1f s, loop from %.1f s, %+.1f dB, decoded in %u ms)",
           job->intro[0] ? job->intro : "", job->intro[0] ? " + " : "", job->path,
           s_track.rate, s_track.channels,
           s_track.rate ? (double)s_track.frames / s_track.rate : 0.0,
           s_track.rate ? (double)s_track.loop_start / s_track.rate : 0.0, job->gain_db,
           job->decode_ms);
  if (s_caption_on)
    snes_osd_caption(s_track.title, CAPTION_MS, s_caption_anchor, HUD_RIGHT_MARGIN);
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

  if (SkipKeyPressed() && s_cur_slot != SLOT_NONE) {
    MusicLog("skip");
    s_skip_pending = true;
  }

  if (s_seek_pending) {
    /* A save state (or rewind) asked for a song at a position. The same
     * song already loaded just moves there; another one replaces it now,
     * without a fade -- the load already cut everything else. */
    if (s_have_track && s_want_slot != SLOT_NONE && !strcmp(s_track.path, s_seek_path)) {
      RtlApuLock();
      s_track.pos = s_seek_pos < s_track.frames ? s_seek_pos : 0;
      s_track.fade_total = s_track.fade_left = 0;
      s_track.finished = s_track.faded = false;
      RtlApuUnlock();
      s_seek_pending = false;
      s_cur_slot = s_want_slot;
      s_cur_stage = s_want_stage;
      s_skip_pending = false;
    } else if (s_want_slot == SLOT_NONE) {
      s_seek_pending = false;
    } else if (s_have_track || s_cur_slot != s_want_slot ||
               s_cur_stage != s_want_stage) {
      /* Once: then RequestLoad below fetches s_seek_path. */
      if (s_have_track)
        FreeTrack();
      s_cur_slot = s_want_slot;
      s_cur_stage = s_want_stage;
      s_skip_pending = false;
      s_job_generation++;                  /* a load in flight is stale now */
    }
  }

  const bool streaming = snes_mod_audio_stream_active();
  const bool change = s_want_slot != s_cur_slot ||
                      (s_want_slot != SLOT_NONE && s_want_stage != s_cur_stage) ||
                      s_skip_pending;
  if (s_have_track && !streaming) {
    if (change || s_track.finished || s_track.faded)
      FreeTrack();                         /* done with it */
    else if (!StartStream())               /* stopped by a reset/load: resume */
      FreeTrack();
  }
  if (change) {
    if (s_have_track) {
      BeginFade();                         /* switch once the fade is done */
    } else {
      if (s_want_slot != s_cur_slot)
        MusicLog("music: %s", s_want_slot == SLOT_NONE ? "original" : kSlotDir[s_want_slot]);
      s_cur_slot = s_want_slot;
      s_cur_stage = s_want_stage;
      s_skip_pending = false;
      s_job_generation++;                  /* a load in flight is stale now */
    }
  }
  if (atomic_load(&s_job_state) == 2)
    InstallLoadedTrack();
  if (!s_have_track && s_cur_slot != SLOT_NONE && s_cur_slot == s_want_slot &&
      !s_skip_pending)
    RequestLoad();

  if (s_log && s_frame % 60 == 0)
    MusicLog("voices sounding: %d, custom: %s", SoundingVoices(),
             s_have_track ? s_track.title : "none");
}

/* ---- reset and save states ---------------------------------------------- */

/* 'STGM': slot and stage. 'STGN' adds the song's file and its position, so
 * a load resumes the same song where it was. */
#define MUSIC_STATE_MAGIC_V1 0x4D475453u   /* 'STGM' */
#define MUSIC_STATE_MAGIC_V2 0x4E475453u   /* 'STGN' */
typedef struct {
  uint32_t magic;
  int32_t slot;
  int32_t stage;
} MusicState;
typedef struct {
  uint32_t pos;
  char path[512];
} MusicStatePosition;
static MusicState s_loaded_state;
static MusicStatePosition s_loaded_position;
static bool s_loaded_state_valid;
static bool s_loaded_position_valid;

void StgCustomMusicOnReset(void)
{
  /* The machine reboots and uploads its sound driver again. */
  s_armed = false;
  s_upload_seen = false;
  s_quiet_frames = 0;
  s_want_slot = SLOT_NONE;
}

/* Called inside the snapshot, with the APU lock held: s_track is stable. */
void StgCustomMusicSaveState(SaveLoadInfo *sli)
{
  MusicState st = { MUSIC_STATE_MAGIC_V2, s_active ? s_want_slot : SLOT_NONE,
                    s_active ? s_want_stage : 0 };
  MusicStatePosition at;
  memset(&at, 0, sizeof(at));
  if (s_active && s_have_track && s_cur_slot == s_want_slot &&
      strlen(s_track.path) < sizeof(at.path)) {
    at.pos = s_track.pos;
    snprintf(at.path, sizeof(at.path), "%s", s_track.path);
  }
  sli->func(sli, &st, sizeof(st));
  sli->func(sli, &at, sizeof(at));
}

void StgCustomMusicLoadState(SaveLoadInfo *sli)
{
  s_loaded_state_valid = false;
  s_loaded_position_valid = false;
  if (RtlStateBytesRemaining(sli) < sizeof(s_loaded_state))
    return;                                /* a state from before music state */
  sli->func(sli, &s_loaded_state, sizeof(s_loaded_state));
  if (s_loaded_state.magic == MUSIC_STATE_MAGIC_V1) {
    s_loaded_state_valid = true;
  } else if (s_loaded_state.magic == MUSIC_STATE_MAGIC_V2 &&
             RtlStateBytesRemaining(sli) >= sizeof(s_loaded_position)) {
    sli->func(sli, &s_loaded_position, sizeof(s_loaded_position));
    s_loaded_position.path[sizeof(s_loaded_position.path) - 1] = '\0';
    s_loaded_state_valid = true;
    s_loaded_position_valid = s_loaded_position.path[0] != '\0';
  }
}

void StgCustomMusicOnStateLoaded(void)
{
  /* A loaded state carries a running sound driver, and says which custom
   * music was playing and where: that song resumes at that point (the
   * frame step loads it if another is playing). Without a position the slot
   * plays on; an older state without the record stops ours. */
  if (!s_active) {
    s_loaded_state_valid = s_loaded_position_valid = false;
    return;
  }
  s_armed = true;
  const bool valid = s_loaded_state_valid;
  const bool at = s_loaded_position_valid;
  s_loaded_state_valid = s_loaded_position_valid = false;
  s_seek_pending = false;
  if (valid && s_loaded_state.slot >= 0 && s_loaded_state.slot < SLOT_COUNT) {
    s_want_slot = s_loaded_state.slot;
    s_want_stage = s_loaded_state.stage;
    if (at) {
      snprintf(s_seek_path, sizeof(s_seek_path), "%s", s_loaded_position.path);
      s_seek_pos = s_loaded_position.pos;
      s_seek_pending = true;
    }
  } else {
    s_want_slot = SLOT_NONE;
  }
}

/* ---- activation ---------------------------------------------------------- */

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
        "  stage-clear    stage clear jingle (used only when \"Keep the original\n"
        "                 jingles\" is off)\n"
        "  game-over      game over / continue jingle (same)\n"
        "  ending         the ending and credits\n"
        "  any            used for any music folder above that is empty\n\n"
        "A folder with several files plays them in order or at random (see the\n"
        "Mods page). A folder with no files, and nothing in any/, keeps the\n"
        "game's original music.\n\n"
        "Formats: WAV, MP3, OGG, FLAC, and MSU-1 .pcm tracks.\n\n"
        "Seamless loops: name a pair  My Song.intro.ogg  and  My Song.loop.ogg\n"
        "(any formats, same sample rate). The intro plays once, then the loop\n"
        "part repeats. Loop points inside WAV files and MSU-1 .pcm files are\n"
        "used too.\n\n"
        "The skip key (N by default, see the Mods page) moves to the next song\n"
        "in the folder that is playing.\n", f);
  fclose(f);
}

static int ScancodeFor(const char *key)
{
  if (!strcmp(key, "n")) return SDL_SCANCODE_N;
  if (!strcmp(key, "m")) return SDL_SCANCODE_M;
  if (!strcmp(key, "backspace")) return SDL_SCANCODE_BACKSPACE;
  if (!strcmp(key, "end")) return SDL_SCANCODE_END;
  return -1;
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
  ReadOption("now_playing", value, sizeof(value), "true");
  s_caption_on = strcmp(value, "false") != 0;
  ReadOption("now_playing_position", value, sizeof(value), "bottom_center");
  s_caption_anchor = strcmp(value, "bottom_right") == 0
      ? SNES_OSD_CAPTION_BOTTOM_RIGHT : SNES_OSD_CAPTION_BOTTOM_CENTER;
  ReadOption("skip_key", value, sizeof(value), "n");
  s_skip_scancode = ScancodeFor(value);
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
  MusicLog("active: folder %s, %s, %s, volume %d%%, jingles %s, loudness %s, "
           "caption %s, skip key %d, decoders mp3 %d ogg %d flac %d",
           s_root, s_random ? "random" : "sequential", s_loop ? "loop" : "next",
           s_volume, s_keep_jingles ? "kept" : "replaceable",
           s_match_loudness ? "matched" : "as recorded",
           s_caption_on ? (s_caption_anchor ? "bottom right" : "bottom centre") : "off",
           s_skip_scancode, MusicDecoderMp3Available(), MusicDecoderOggAvailable(),
           MusicDecoderFlacAvailable());
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
