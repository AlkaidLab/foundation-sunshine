#pragma once
#include "../../third-party/moonlight-common-c/src/MatLivePlayback.h"
#ifdef __cplusplus
extern "C" {
#endif
int
LiveCheckOpen(const LI_MAT_LIVE_PROFILE *, const uint8_t key[16], const LI_MAT_PLAYBACK_CALLBACKS *, int drop);
int
LiveCheckSend(const uint8_t *, size_t);
uint64_t
LiveCheckDatagrams(void);
uint64_t
LiveCheckDrops(void);
uint64_t
LiveCheckRecovered(void);
void
LiveCheckClose(void);
#ifdef __cplusplus
}
#endif
