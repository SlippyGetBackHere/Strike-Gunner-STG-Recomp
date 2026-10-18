#ifndef GAME_RTL_H
#define GAME_RTL_H

#include "common_cpu_infra.h"

/* One frame of Strike Gunner S.T.G. See game_rtl.c — this is the port. */
void GameRunOneFrame(void);
void GameDrawPpuFrame(void);
void GameSessionReset(void);

/* Master clock at which the field being run ends (0 outside a frame). */
uint64_t GameFrameDeadline(void);

/* A stub handed a routine back to the driver: resume it without parking. */
void GameRequestImmediateResume(void);

/* Infinite-lives plugin (mod_infinite_lives.c): drop the remembered counts
 * after a state load or rewind, so going back is never "corrected". */
void StgInfiniteLivesForget(void);

/* Custom-music plugin (mod_custom_music.c): the machine rebooted (its sound
 * driver is uploaded again), or a save state was loaded. */
void StgCustomMusicOnReset(void);
void StgCustomMusicOnStateLoaded(void);
/* Which custom music is playing, kept in save states after the STG1 chunk
 * (a state from before it simply lacks the record). */
struct SaveLoadInfo;
void StgCustomMusicSaveState(struct SaveLoadInfo *sli);
void StgCustomMusicLoadState(struct SaveLoadInfo *sli);

extern const RtlGameInfo kGameInfo;

#endif /* GAME_RTL_H */
