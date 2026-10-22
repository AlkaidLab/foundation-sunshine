package com.limelight.binding.audio;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;
import android.os.Bundle;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Arrays;

/** Explicit ADB diagnostic: real device ART/JNI, fake carrier output, no network. */
public final class MatDeviceCheck extends Instrumentation {
    private boolean playResearchPcm;
    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        playResearchPcm=arguments!=null && "true".equals(arguments.getString("playResearchPcm"));
        start();
    }

    @Override public void onStart() {
        Bundle result = new Bundle();
        File fixture = null;
        try {
            if (Build.VERSION.SDK_INT < 26) throw new IllegalStateException("Device checks need API 26+ (java.nio.file)");
            Context context = getTargetContext();
            result.putString("device", Build.MANUFACTURER + " " + Build.MODEL);
            result.putInt("api", Build.VERSION.SDK_INT);
            result.putString("abis", Arrays.toString(Build.SUPPORTED_ABIS));
            fixture = File.createTempFile("mat-device-", ".raw", context.getCacheDir());
            try (InputStream in = context.getAssets().open("verified-mat.raw");
                 FileOutputStream out = new FileOutputStream(fixture)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
            }
            // Same negative, lifecycle, byte-order, digest and real-time JNI tests
            // as the desktop run; only the final sink is intentionally fake.
            MatOfflineTest.main(new String[]{fixture.getAbsolutePath()});
            result.putString("jniPlayer", "PASS: production receiver/player; fake final sink");
            result.putString("fixtureSha256", MatRecording.VERIFIED_SHA256);

            AudioManager manager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
            StringBuilder outputs = new StringBuilder();
            for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                outputs.append("type=").append(device.getType())
                        .append(" encodings=").append(Arrays.toString(device.getEncodings())).append(';');
            }
            result.putString("outputs", outputs.toString());
            StringBuilder audioDecoders = new StringBuilder();
            for (MediaCodecInfo codec : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
                if (codec.isEncoder()) continue;
                for (String mime : codec.getSupportedTypes()) {
                    if (mime.startsWith("audio/")) audioDecoders.append(codec.getName()).append(':').append(mime).append(';');
                }
            }
            result.putString("audioDecoders", audioDecoders.toString());
            for (String mime : new String[]{"audio/vnd.dolby.mat", "audio/vnd.dolby.mlp", "audio/eac3-joc", "audio/eac3", "audio/ac4"}) {
                try {
                    MediaCodec decoder = MediaCodec.createDecoderByType(mime);
                    try { result.putString("decoder_" + mime, "CREATED: " + decoder.getName() + "; decode not tested"); }
                    finally { decoder.release(); }
                } catch (java.io.IOException | IllegalArgumentException unavailable) {
                    result.putString("decoder_" + mime, "UNAVAILABLE: " + unavailable);
                }
            }
            // Exercise real platform setup without submitting any carrier bytes.
            // An opened track is still not evidence of a working HDMI MAT route.
            MatAudioTrackSink sink = new MatAudioTrackSink(context);
            try {
                sink.start();
                result.putString("sinkSetup", "OPENED, no bytes submitted; " + sink.diagnostics());
            } catch (RuntimeException unsupported) {
                result.putString("sinkSetup", "REJECTED, no bytes submitted: " + unsupported);
            } finally {
                sink.close();
            }
            result.putString("scope", "No network session, audible playback or Atmos recognition tested");
            if (playResearchPcm) {
                result.putString("speakerAudition", MatResearchSpeaker.play(context));
                result.putString("scope", "Fixed-fixture research PCM submitted to speaker; no network or Atmos rendering");
            }
            result.putString("result", "PASS");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable failure) {
            result.putString("result", "FAIL");
            result.putString("failure", android.util.Log.getStackTraceString(failure));
            finish(Activity.RESULT_CANCELED, result);
        } finally {
            if (fixture != null) fixture.delete();
        }
    }
}
