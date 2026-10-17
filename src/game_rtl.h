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

extern const RtlGameInfo kGameInfo;

#endif /* GAME_RTL_H */
