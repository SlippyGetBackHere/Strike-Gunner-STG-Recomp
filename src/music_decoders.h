/*
 * Compressed-audio decoding for the Custom music mod: MP3, Ogg Vorbis and
 * FLAC, each available when its single-file decoder is compiled in (see
 * music_decoders.c). Whole-file decode to interleaved signed-16, at most
 * stereo; called on the mod's worker thread.
 */
#ifndef STG_MUSIC_DECODERS_H
#define STG_MUSIC_DECODERS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct MusicDecodedAudio {
  int16_t *samples;      /* malloc'd; the caller frees */
  uint32_t frames;
  uint32_t channels;     /* 1 or 2 */
  uint32_t sample_rate;
} MusicDecodedAudio;

bool MusicDecoderMp3Available(void);
bool MusicDecoderOggAvailable(void);
bool MusicDecoderFlacAvailable(void);

/* By extension (.mp3, .ogg, .flac). False on an unsupported or bad file. */
bool MusicDecodeCompressed(const char *path, MusicDecodedAudio *out);

#endif /* STG_MUSIC_DECODERS_H */
