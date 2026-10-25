package com.sunshine.matmock;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.SystemClock;
import org.json.JSONObject;
import java.nio.ByteBuffer;
import static com.sunshine.matmock.MatInstrumentation.check;

/** Full common-c session. Video is decoded in memory; audio has no device output. */
final class Session {
    private static native long[] runNative(Audio audio, Video video, String[] arguments);
    static final class Audio {
        MatInstrumentation.Output output;
        int codec, opusPackets;
        long first, last;
        public boolean init(int codec, long epoch, long domain) throws Exception {
            this.codec = codec;
            if (codec == 7) output = new MatInstrumentation.Output(epoch, 0, domain, false);
            return codec == 0 || codec == 7;
        }
        void timestamp() { last = SystemClock.elapsedRealtime(); if (first == 0) first = last; }
        public boolean cycle(ByteBuffer bytes, long epoch, long domain, long index, long pts) {
            timestamp(); return output != null && output.onCycle(bytes, epoch, domain, index, pts);
        }
        public boolean end(long count) { return output != null && output.onEnd(count); }
        public boolean opus(ByteBuffer bytes) {
            check(codec == 0 && bytes.remaining() > 0, "unexpected compressed audio callback");
            ++opusPackets; timestamp(); return true; // Encoded Opus is discarded by the mock.
        }
        public void close() { if (output != null) output.close(); }
    }
    static final class Video {
        MediaCodec decoder;
        final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int frames, units, width, height;
        String name = "";
        long first, last, lastPts = -1, inputPts;
        boolean reordered;
        public boolean setup(int format, int width, int height, int fps) throws Exception {
            check(format == 1 && width == 640 && height == 360 && fps == 30, "H264 session format");
            decoder = MediaCodec.createDecoderByType("video/avc");
            name = decoder.getName();
            MediaFormat config = MediaFormat.createVideoFormat("video/avc", width, height);
            config.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4 * 1024 * 1024);
            decoder.configure(config, null, null, 0); decoder.start(); return true;
        }
        void drain(long timeout) {
            for (;;) {
                int index = decoder.dequeueOutputBuffer(info, timeout); timeout = 0;
                if (index == MediaCodec.INFO_TRY_AGAIN_LATER) return;
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat format = decoder.getOutputFormat();
                    width = format.getInteger(MediaFormat.KEY_WIDTH); height = format.getInteger(MediaFormat.KEY_HEIGHT);
                    check(width == 640 && height == 360, "decoded dimensions");
                } else if (index >= 0) {
                    if (info.size > 0) {
                        reordered |= lastPts >= 0 && info.presentationTimeUs < lastPts;
                        lastPts = info.presentationTimeUs; ++frames;
                        last = SystemClock.elapsedRealtime(); if (first == 0) first = last;
                    }
                    decoder.releaseOutputBuffer(index, false);
                }
            }
        }
        public boolean submit(ByteBuffer bytes, long pts) {
            drain(0); int index = decoder.dequeueInputBuffer(10000);
            // Codec startup may hold every input buffer while producing output.
            // Keep this unit and release output until an input returns or we fail.
            long deadline = SystemClock.elapsedRealtime() + 500;
            while (index < 0 && SystemClock.elapsedRealtime() < deadline) {
                drain(10000); index = decoder.dequeueInputBuffer(10000);
            }
            check(index >= 0, "MediaCodec input timeout");
            ByteBuffer input = decoder.getInputBuffer(index); input.clear();
            int size = bytes.remaining(); check(input.remaining() >= size, "MediaCodec input capacity");
            input.put(bytes); decoder.queueInputBuffer(index, 0, size, pts, 0);
            inputPts = pts; ++units; drain(0); return true;
        }
        public void close() {
            if (decoder == null) return;
            try {
                int index = decoder.dequeueInputBuffer(10000);
                if (index >= 0) decoder.queueInputBuffer(index, 0, 0, inputPts + 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                for (int i = 0; i < 20; ++i) drain(10000);
            } finally { decoder.stop(); decoder.release(); decoder = null; }
        }
    }
    static JSONObject run(Bundle options) throws Exception {
        Audio audio = new Audio(); Video video = new Video();
        String[] keys = {"sessionHost", "sessionUrl", "appVersion", "gfeVersion", "serverCodecMask",
            "inputKey", "inputIv", "audioCodec", "seconds"};
        String[] arguments = new String[keys.length];
        for (int i = 0; i < keys.length; ++i) arguments[i] = options.getString(keys[i]);
        long[] status;
        try { status = runNative(audio, video, arguments); }
        finally { video.close(); audio.close(); }
        int expectedCodec = Integer.parseInt(options.getString("expectedCodec"));
        long overlap = Math.max(0, Math.min(audio.last, video.last) - Math.max(audio.first, video.first));
        boolean mat = audio.codec == 7;
        String accepted = "", consumed = "";
        if (mat) {
            accepted = MatInstrumentation.hex(audio.output.expected.digest());
            consumed = MatInstrumentation.hex(audio.output.consumed.digest());
        }
        boolean drained = mat && audio.output.completed && accepted.equals(consumed);
        boolean passed = status[0] == 0 && status[1] == expectedCodec && status[2] == 1 && status[4] == 0 &&
            video.frames >= 30 && !video.reordered && overlap >= 1000 &&
            (mat ? status[3] == 1 && drained && audio.output.cycles >= 100 : audio.opusPackets >= 30);
        return new JSONObject().put("passed", passed).put("codec", status[1]).put("encrypted", status[2] == 1)
            .put("authenticatedEOS", status[3] == 1).put("nativeFailed", status[4] != 0)
            .put("fullLiStartConnection", status[0] == 0).put("startError", status[0])
            .put("videoDecoder", video.name).put("videoFramesDecoded", video.frames).put("videoUnits", video.units)
            .put("videoPtsReordered", video.reordered).put("videoWidth", video.width).put("videoHeight", video.height)
            .put("opusPackets", audio.opusPackets).put("overlapMs", overlap).put("pid", android.os.Process.myPid())
            .put("cycles", mat ? audio.output.cycles : 0).put("sha256", accepted)
            .put("mockWrittenSha256", consumed).put("mockPlayedSha256", consumed)
            .put("mockPlayedFrames", mat ? audio.output.consumedBytes / 16 : 0).put("mockOutputCompleted", drained)
            .put("audioDecodeCalls", mat ? audio.opusPackets : 0).put("hardwareSink", false).put("avSyncCalibrated", false);
    }
}
