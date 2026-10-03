#include "Limelight-internal.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* video;
static FILE* frames;
static atomic_uint frameCount;
static atomic_uint_fast64_t audioPacketCount;
static atomic_uint_fast64_t audioBytes;
static atomic_int terminationCode;
static atomic_bool captureIoFailed;
static atomic_bool captureTruncated;
static uint64_t captureBytes;

static bool
closeCapture(void) {
  bool valid = !atomic_load_explicit(&captureIoFailed, memory_order_relaxed);
  if (video) {
    if (ferror(video)) valid = false;
    if (fclose(video) != 0) valid = false;
    video = NULL;
  }
  if (frames) {
    if (ferror(frames)) valid = false;
    if (fclose(frames) != 0) valid = false;
    frames = NULL;
  }
  return valid;
}

static void logMessage(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

static int videoSetup(int format, int width, int height, int rate, void* context, int flags) {
    (void)context; (void)flags;
    fprintf(stderr, "Video format %d, %dx%d at %d fps\n", format, width, height, rate);
    return format == VIDEO_FORMAT_H264 ? 0 : -1;
}

static int submit(PDECODE_UNIT unit) {
  if (atomic_load_explicit(&captureTruncated, memory_order_relaxed)) return DR_NEED_IDR;
  if (captureBytes + (uint64_t) unit->fullLength > 100 * 1024 * 1024) {
    atomic_store_explicit(&captureTruncated, true, memory_order_relaxed);
    return DR_NEED_IDR;
  }
    for (PLENTRY entry = unit->bufferList; entry != NULL; entry = entry->next) {
      if (fwrite(entry->data, 1, (size_t) entry->length, video) != (size_t) entry->length) {
        atomic_store_explicit(&captureIoFailed, true, memory_order_relaxed);
        return DR_NEED_IDR;
      }
    }
    captureBytes += (uint64_t)unit->fullLength;
    if (fprintf(frames, "%d,%d,%d,%" PRIu64 ",%" PRIu64 "\n", unit->frameNumber,
          unit->frameType, unit->fullLength, unit->receiveTimeUs, PltGetMicroseconds()) < 0) {
      atomic_store_explicit(&captureIoFailed, true, memory_order_relaxed);
      return DR_NEED_IDR;
    }
    atomic_fetch_add_explicit(&frameCount, 1, memory_order_relaxed);
    return DR_OK;
}

static int audioInit(int configuration, const POPUS_MULTISTREAM_CONFIGURATION opus, void* context, int flags) {
    (void)configuration; (void)opus; (void)context; (void)flags;
    return 0;
}
static void audioSample(char* sample, int bytes) {
    (void)sample;
    if (bytes > 0) {
        atomic_fetch_add_explicit(&audioPacketCount, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&audioBytes, (uint64_t)bytes, memory_order_relaxed);
    }
}
static void terminated(int error) { atomic_store_explicit(&terminationCode, error ? error : -1, memory_order_relaxed); }

int main(int argc, char** argv) {
    if (argc != 10 || strlen(argv[8]) != 32) {
        fprintf(stderr, "Usage: client host rtsp-url app-version gfe-version codec-mask seconds feedback-enabled key-hex capture-prefix\n");
        return 2;
    }
    const int seconds = atoi(argv[6]);
    if (seconds < 1 || seconds > 60) return 2;
    char path[1024];
    if (snprintf(path, sizeof(path), "%s.h264", argv[9]) >= (int)sizeof(path)) return 2;
    video = fopen(path, "wb");
    if (snprintf(path, sizeof(path), "%s.frames.csv", argv[9]) >= (int)sizeof(path)) return 2;
    frames = fopen(path, "w");
    if (!video || !frames) {
      (void) closeCapture();
      return 2;
    }
    if (fprintf(frames, "frame,type,bytes,first_receive_us,submit_us\n") < 0) {
      (void) closeCapture();
      return 2;
    }
    SERVER_INFORMATION host;
    LiInitializeServerInformation(&host);
    host.address = argv[1]; host.rtspSessionUrl = argv[2];
    host.serverInfoAppVersion = argv[3]; host.serverInfoGfeVersion = argv[4];
    host.serverCodecModeSupport = atoi(argv[5]);
    STREAM_CONFIGURATION configuration;
    LiInitializeStreamConfiguration(&configuration);
    configuration.width = 1280; configuration.height = 720; configuration.fps = 30;
    configuration.bitrate = 10000; configuration.packetSize = 1392;
    configuration.streamingRemotely = STREAM_CFG_LOCAL;
    configuration.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    configuration.supportedVideoFormats = VIDEO_FORMAT_H264;
    configuration.encryptionFlags = ENCFLG_ALL;
    configuration.remoteInputAesIv[3] = 1;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned value;
        if (sscanf(argv[8] + i * 2, "%2x", &value) != 1) return 2;
        configuration.remoteInputAesKey[i] = (char)value;
    }
    DECODER_RENDERER_CALLBACKS decoder;
    LiInitializeVideoCallbacks(&decoder);
    decoder.setup = videoSetup; decoder.submitDecodeUnit = submit;
    decoder.capabilities = CAPABILITY_DIRECT_SUBMIT;
    AUDIO_RENDERER_CALLBACKS audio;
    LiInitializeAudioCallbacks(&audio);
    audio.init = audioInit; audio.decodeAndPlaySample = audioSample;
    audio.capabilities = CAPABILITY_DIRECT_SUBMIT;
    CONNECTION_LISTENER_CALLBACKS callbacks;
    LiInitializeConnectionCallbacks(&callbacks);
    callbacks.logMessage = logMessage; callbacks.connectionTerminated = terminated;
    if (!LiSetVideoPacketFeedbackEnabled(atoi(argv[7]) == 1) || !LiSetVideoNetworkObservationEnabled(true)) return 2;
    const char* control_request = getenv("TRANSPORT_TEST_PACKET_CONTROL");
    const bool requested_control = control_request != NULL && strcmp(control_request, "1") == 0;
    if (!LiSetVideoPacketControlEnabled(requested_control)) return 2;
    const int result = LiStartConnection(&host, &configuration, &callbacks, &decoder, &audio, NULL, 0, NULL, 0);
    if (result) {
      fprintf(stderr, "Connection failed: %d\n", result);
      (void) closeCapture();
      return 3;
    }
    const uint64_t epoch = VideoPacketFeedbackConnectionEpoch;
    const bool negotiated_control = LiGetVideoPacketControlNegotiated();
    uint64_t notice_sequence = 0;
    unsigned notices_observed = 0;
    for (int i = 0; i < seconds * 10 && !atomic_load_explicit(&terminationCode, memory_order_relaxed) &&
                    !atomic_load_explicit(&captureIoFailed, memory_order_relaxed) &&
                    !atomic_load_explicit(&captureTruncated, memory_order_relaxed);
      ++i) {
      LI_VIDEO_NETWORK_SNAPSHOT snapshot;
      if (LiGetVideoNetworkSnapshot(&snapshot)) {
        printf("{\"epoch\":\"%" PRIu64 "\",\"sample_us\":\"%" PRIu64
               "\",\"packets\":\"%" PRIu64 "\",\"bytes\":\"%" PRIu64
               "\",\"invalid\":\"%" PRIu64 "\",\"auth_failures\":\"%" PRIu64
               "\",\"missing_candidates\":\"%" PRIu64 "\",\"recovered\":\"%" PRIu64 "\"}\n",
          snapshot.connectionEpoch, snapshot.sampleTimeUs, snapshot.uniquePackets, snapshot.uniqueUdpBytes,
          snapshot.invalidPackets, snapshot.authenticationFailures, snapshot.missingCandidates, snapshot.recoveredDataPackets);
        fflush(stdout);
      }
      TPS_STATUS_NOTICE notice;
      if (LiGetTransportPolicyStatusNotice(&notice)) {
        if (notice.noticeSequence != notice_sequence) notices_observed++;
        notice_sequence = notice.noticeSequence;
        printf("{\"policy_notice\":true,\"sample_us\":\"%" PRIu64
               "\",\"sessionId\":\"%" PRIu32 "\",\"connectionEpoch\":\"%" PRIu64
               "\",\"sequence\":\"%" PRIu64 "\",\"controlEpoch\":\"%" PRIu64
               "\",\"acceptedRevision\":\"%" PRIu64 "\",\"appliedRevision\":\"%" PRIu64
               "\",\"firstSentRevision\":\"%" PRIu64 "\",\"firstSentFrame\":\"%" PRIu64
               "\",\"flags\":%u,\"source\":%u,\"failure\":%u}\n",
          PltGetMicroseconds(), notice.sessionId, notice.connectionEpoch, notice.noticeSequence,
          notice.controlEpoch, notice.acceptedRevision, notice.encoderAppliedRevision,
          notice.firstSentRevision, notice.firstSentFrame,
          notice.flags, notice.controlSource, notice.failure);
        fflush(stdout);
      }
      PltSleepMs(100);
    }
    LiStopConnection();
    TPS_STATUS_NOTICE stopped_notice;
    const bool notice_after_stop = LiGetTransportPolicyStatusNotice(&stopped_notice);
    const bool outputs_valid = closeCapture();
    const bool truncated = atomic_load_explicit(&captureTruncated, memory_order_relaxed);
    printf("{\"frames\":%u,\"video_bytes\":\"%" PRIu64 "\",\"negotiated_epoch\":\"%" PRIu64
           "\",\"control_requested\":%s,\"control_negotiated\":%s,\"termination\":%d,\"audio_packets_delivered\":\"%" PRIu64 "\",\"audio_bytes_delivered\":\"%" PRIu64
           "\",\"notices_observed\":%u,\"notice_after_stop\":%s,\"capture_io_failed\":%s,\"capture_truncated\":%s}\n",
      atomic_load_explicit(&frameCount, memory_order_relaxed), captureBytes, epoch,
      requested_control ? "true" : "false", negotiated_control ? "true" : "false",
      atomic_load_explicit(&terminationCode, memory_order_relaxed),
      (uint64_t) atomic_load_explicit(&audioPacketCount, memory_order_relaxed),
      (uint64_t) atomic_load_explicit(&audioBytes, memory_order_relaxed),
      notices_observed, notice_after_stop ? "true" : "false", outputs_valid ? "false" : "true",
      truncated ? "true" : "false");
    return outputs_valid && atomic_load_explicit(&frameCount, memory_order_relaxed) &&
               (atoi(argv[7]) != 1 || epoch != 0) && (!requested_control || negotiated_control) &&
               !truncated && !notice_after_stop && !atomic_load_explicit(&terminationCode, memory_order_relaxed) ?
             0 :
             4;
}
