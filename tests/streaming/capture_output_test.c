/* Exercise the capture tool's actual callbacks against failing stdio streams. */
#define main transport_stream_client_main
#include "stream_client.c"
#undef main

static bool
run_case(int failure) {
  atomic_store(&captureIoFailed, false);
  atomic_store(&captureTruncated, false);
  atomic_store(&frameCount, 0);
  captureBytes = 0;
  video = failure == 1 ? fopen(__FILE__, "rb") : tmpfile();
  frames = failure == 2 ? fopen(__FILE__, "rb") : tmpfile();
#if defined(__linux__)
  if (failure == 3 || failure == 4) {
    FILE **sink = failure == 3 ? &video : &frames;
    if (*sink) fclose(*sink);
    *sink = fopen("/dev/full", "wb");
    if (*sink && setvbuf(*sink, NULL, _IOFBF, 4096) != 0) {
      (void) closeCapture();
      return false;
    }
  }
#endif
  if (!video || !frames) {
    (void) closeCapture();
    return false;
  }
  char payload[] = "capture";
  LENTRY entry = { 0 };
  entry.data = payload;
  entry.length = (int) sizeof(payload);
  DECODE_UNIT unit = { 0 };
  unit.bufferList = &entry;
  unit.fullLength = entry.length;
  unit.frameNumber = 1;
  const int submitted = submit(&unit);
  const bool closed = closeCapture();
  const bool immediate_error = failure == 1 || failure == 2;
  return video == NULL && frames == NULL && closed == (failure == 0) &&
         submitted == (immediate_error ? DR_NEED_IDR : DR_OK) &&
         atomic_load(&frameCount) == (immediate_error ? 0u : 1u);
}

static bool
run_limit_case(void) {
  atomic_store(&captureIoFailed, false);
  atomic_store(&captureTruncated, false);
  atomic_store(&frameCount, 0);
  video = tmpfile();
  frames = tmpfile();
  if (!video || !frames) {
    (void) closeCapture();
    return false;
  }
  char payload[] = "capture";
  LENTRY entry = { 0 };
  entry.data = payload;
  entry.length = (int) sizeof(payload);
  DECODE_UNIT unit = { 0 };
  unit.bufferList = &entry;
  unit.fullLength = entry.length;
  captureBytes = 100 * 1024 * 1024 - sizeof(payload);
  bool valid = submit(&unit) == DR_OK && !atomic_load(&captureTruncated) &&
               captureBytes == 100 * 1024 * 1024 && atomic_load(&frameCount) == 1;
  const long frame_position = ftell(frames);
  valid = valid && frame_position >= 0 && submit(&unit) == DR_NEED_IDR &&
          atomic_load(&captureTruncated) && !atomic_load(&captureIoFailed) &&
          captureBytes == 100 * 1024 * 1024 && atomic_load(&frameCount) == 1 &&
          ftell(video) == (long) sizeof(payload) && ftell(frames) == frame_position;
  valid = valid && submit(&unit) == DR_NEED_IDR && atomic_load(&frameCount) == 1 &&
          ftell(video) == (long) sizeof(payload) && ftell(frames) == frame_position;
  const bool closed = closeCapture();
  return valid && closed;
}

int
main(void) {
  int count = 3;
#if defined(__linux__)
  FILE *full = fopen("/dev/full", "rb");
  if (full) {
    if (fclose(full) != 0) return 1;
    count = 5;
  }
#endif
  for (int failure = 0; failure < count; ++failure) {
    if (!run_case(failure)) {
      fprintf(stderr, "Capture I/O case %d failed\n", failure);
      return 1;
    }
  }
  if (!run_limit_case()) {
    fprintf(stderr, "Capture byte limit case failed\n");
    return 1;
  }
  printf("Capture I/O: %d cases and byte limit boundary passed\n", count);
  return 0;
}
