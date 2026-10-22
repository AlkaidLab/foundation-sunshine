package com.limelight.binding.audio;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import java.io.InputStream;

/** Explicit, low-volume PCM audition of one known recording; never writes carrier bytes as PCM. */
public final class MatResearchSpeaker {
    public static String play(Context context) throws Exception {
        short[] pcm;
        try (InputStream input=context.getAssets().open("verified-mat.raw")) {
            pcm=MatResearchPcm.render(MatResearchPcm.load(input));
        }
        String extractionSha256=MatResearchPcm.sha256(pcm);
        int extractionPeak=MatResearchPcm.peak(pcm);
        pcm=MatResearchPcm.audition(pcm);
        AudioManager manager=(AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
        AudioDeviceInfo speaker=null;
        for (AudioDeviceInfo d:manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
            if (d.getType()==AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) speaker=d;
        if (speaker==null) throw new IllegalStateException("No built-in speaker");
        int min=AudioTrack.getMinBufferSize(48000,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT);
        if (min<=0) throw new IllegalStateException("PCM speaker tuple unsupported");
        AudioTrack track=new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(min,19200)).build();
        try {
            if (track.getState()!=AudioTrack.STATE_INITIALIZED || !track.setPreferredDevice(speaker))
                throw new IllegalStateException("Cannot select built-in speaker");
            if (track.setVolume(0.35f)!=AudioTrack.SUCCESS) throw new IllegalStateException("Cannot set audition volume");
            track.play();
            long deadline=System.nanoTime()+8_000_000_000L;
            int offset=0;
            while (offset<pcm.length) {
                if (Thread.currentThread().isInterrupted()) throw new InterruptedException("Audition cancelled");
                if (System.nanoTime()>deadline) throw new IllegalStateException("PCM write timeout");
                int requested=Math.min(1920,pcm.length-offset);
                int n=track.write(pcm,offset,requested,AudioTrack.WRITE_NON_BLOCKING);
                if (n<0 || n>requested || n%2!=0) throw new IllegalStateException("PCM write error: "+n);
                offset+=n;
                AudioDeviceInfo routed=track.getRoutedDevice();
                if (routed!=null && routed.getId()!=speaker.getId()) throw new IllegalStateException("Audition left speaker route");
                if (n==0) Thread.sleep(2);
            }
            while ((track.getPlaybackHeadPosition()&0xffffffffL)<pcm.length/2) {
                if (Thread.currentThread().isInterrupted()) throw new InterruptedException("Audition cancelled");
                if (System.nanoTime()>deadline) throw new IllegalStateException("PCM drain timeout");
                Thread.sleep(5);
            }
            AudioDeviceInfo routed=track.getRoutedDevice();
            if (routed==null || routed.getId()!=speaker.getId()) throw new IllegalStateException("Speaker route unconfirmed");
            return "frames="+(pcm.length/2)+"; rate=48000; channels=2; speakerType="+routed.getType()+
                    "; playhead="+(track.getPlaybackHeadPosition()&0xffffffffL)+"; trackVolume=0.35; extractionPeak="+
                    extractionPeak+"; auditionPeak="+MatResearchPcm.peak(pcm)+"; extractionSha256="+extractionSha256+
                    "; submittedPcmSha256="+MatResearchPcm.sha256(pcm)+"; fixed-fixture mono audition, no spatial metadata rendering";
        } finally {
            try {track.pause();track.flush();} finally {track.release();}
        }
    }
}
