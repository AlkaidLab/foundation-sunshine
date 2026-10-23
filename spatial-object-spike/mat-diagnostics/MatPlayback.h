#pragma once
#include "../../third-party/moonlight-common-c/src/Limelight.h"
#include "MatDumpProfile.h"
typedef struct MAT_PLAYBACK_RECEIVER MAT_PLAYBACK_RECEIVER;
MAT_PLAYBACK_RECEIVER* MatPlaybackOpen(const LI_MAT_DUMP_PROFILE*,const LI_MAT_PLAYBACK_CALLBACKS*);
int MatPlaybackPush(MAT_PLAYBACK_RECEIVER*,const unsigned char*,size_t);
void MatPlaybackFail(MAT_PLAYBACK_RECEIVER*);
void MatPlaybackClose(MAT_PLAYBACK_RECEIVER*);
