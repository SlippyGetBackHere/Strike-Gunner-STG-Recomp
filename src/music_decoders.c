/*
 * Compressed-audio decoding for the Custom music mod. Each format is built
 * in when its public-domain single-file decoder is present under
 * third_party/audio/ (CMakeLists.txt defines STG_HAVE_<FORMAT> for it):
 *
 *   dr_mp3.h, dr_flac.h   https://github.com/mackron/dr_libs
 *   stb_vorbis.c          https://github.com/nothings/stb
 *
 * Without a decoder the format is simply not offered: its files are not
 * listed as playable, so a folder holding only those falls back as if empty.
 */
#include "music_decoders.h"

#include <stdlib.h>
#include <string.h>

#if defined(STG_HAVE_DR_MP3)
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#endif
#if defined(STG_HAVE_DR_FLAC)
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#endif
#if defined(STG_HAVE_STB_VORBIS)
#include "stb_vorbis.c"
#endif

bool MusicDecoderMp3Available(void)
{
#if defined(STG_HAVE_DR_MP3)
  return true;
#else
  return false;
#endif
}

bool MusicDecoderOggAvailable(void)
{
#if defined(STG_HAVE_STB_VORBIS)
  return true;
#else
  return false;
#endif
}

bool MusicDecoderFlacAvailable(void)
{
#if defined(STG_HAVE_DR_FLAC)
  return true;
#else
  return false;
#endif
}

static bool HasExtension(const char *path, const char *ext)
{
  const size_t n = strlen(path), e = strlen(ext);
  if (n <= e)
    return false;
  for (size_t i = 0; i < e; i++) {
    char a = path[n - e + i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (a != ext[i])
      return false;
  }
  return true;
}

/* Keep at most two channels, in place. */
static void FoldToStereo(int16_t *pcm, uint64_t frames, uint32_t *channels)
{
  if (*channels <= 2)
    return;
  const uint32_t ch = *channels;
  for (uint64_t i = 0; i < frames; i++) {
    pcm[i * 2] = pcm[i * ch];
    pcm[i * 2 + 1] = pcm[i * ch + 1];
  }
  *channels = 2;
}

bool MusicDecodeCompressed(const char *path, MusicDecodedAudio *out)
{
  memset(out, 0, sizeof(*out));
  int16_t *pcm = NULL;
  uint64_t frames = 0;
  uint32_t channels = 0, rate = 0;
  (void)pcm; (void)frames; (void)channels; (void)rate;
#if defined(STG_HAVE_DR_MP3)
  if (HasExtension(path, ".mp3")) {
    drmp3_config cfg;
    drmp3_uint64 n = 0;
    pcm = drmp3_open_file_and_read_pcm_frames_s16(path, &cfg, &n, NULL);
    frames = n; channels = cfg.channels; rate = cfg.sampleRate;
  }
#endif
#if defined(STG_HAVE_DR_FLAC)
  if (HasExtension(path, ".flac")) {
    unsigned int ch = 0, sr = 0;
    drflac_uint64 n = 0;
    pcm = drflac_open_file_and_read_pcm_frames_s16(path, &ch, &sr, &n, NULL);
    frames = n; channels = ch; rate = sr;
  }
#endif
#if defined(STG_HAVE_STB_VORBIS)
  if (HasExtension(path, ".ogg")) {
    int ch = 0, sr = 0;
    short *out_pcm = NULL;
    const int n = stb_vorbis_decode_filename(path, &ch, &sr, &out_pcm);
    if (n > 0) {
      pcm = out_pcm; frames = (uint64_t)n; channels = (uint32_t)ch; rate = (uint32_t)sr;
    } else {
      free(out_pcm);
    }
  }
#endif
  (void)HasExtension;
  if (!pcm || !frames || !channels || rate < 8000 || rate > 192000 ||
      frames > 0xFFFFFFFFu) {
    free(pcm);
    return false;
  }
  FoldToStereo(pcm, frames, &channels);
  out->samples = pcm;
  out->frames = (uint32_t)frames;
  out->channels = channels;
  out->sample_rate = rate;
  return true;
}
