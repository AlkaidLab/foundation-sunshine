#pragma once
#include "../../third-party/moonlight-common-c/src/MatDumpProfile.h"
#include "../../third-party/moonlight-common-c/src/Limelight.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MAT_DUMP_FILE MAT_DUMP_FILE;
MAT_DUMP_FILE* MatDumpOpen(const char* final_path,const LI_MAT_DUMP_PROFILE* expected);
LI_MAT_PLAYBACK_CALLBACKS MatDumpCallbacks(MAT_DUMP_FILE*);
int MatDumpPush(MAT_DUMP_FILE*,const unsigned char*,size_t); // -1 failed, 0 pending, 2 published
void MatDumpFail(MAT_DUMP_FILE*);
int MatDumpPublished(const MAT_DUMP_FILE*);
void MatDumpClose(MAT_DUMP_FILE*); // Missing EOS remains .partial; never overwrites.
#ifdef __cplusplus
}
#endif
