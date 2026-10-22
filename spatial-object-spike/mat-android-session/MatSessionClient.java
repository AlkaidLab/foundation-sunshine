package com.limelight.binding.audio;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;

/** JNI session frontend. Uses production MatPlaybackSession; only AudioTrack is replaced. */
public final class MatSessionClient {
    static { System.loadLibrary("moonlight-mat-session"); }
    private static native int nativeRun(String[] arguments,MatCarrierConsumer consumer);
    private static final class FakeSink implements MatCarrierPlayer.Sink {
        final MessageDigest digest=MessageDigest.getInstance("SHA-256");
        long bytes; int calls; boolean closed;
        FakeSink() throws Exception {}
        @Override public void start() {}
        @Override public int write(short[] words,int offset,int count) {
            if(++calls%17==0)return 0;
            int n=Math.min(count,1024);
            byte[] data=new byte[n*2];
            for(int i=0;i<n;i++){data[2*i]=(byte)words[offset+i];data[2*i+1]=(byte)(words[offset+i]>>>8);}
            digest.update(data);bytes+=data.length;return n;
        }
        @Override public long playedFrames(){return bytes/16;}
        @Override public void close(){closed=true;}
    }
    public static void main(String[] args) throws Exception {
        boolean rejection=args.length==9 && args[8].equals("reject");
        if(args.length!=8 && !rejection)throw new IllegalArgumentException("Session arguments required");
        FakeSink sink=new FakeSink();
        try(MatPlaybackSession session=new MatPlaybackSession(sink)) {
            int result=nativeRun(java.util.Arrays.copyOf(args,8),session);
            if(rejection) {
                if(result!=3 || sink.bytes!=0)throw new IllegalStateException("Invalid profile unexpectedly accepted: "+result);
                System.out.println("PASS EXPECTED_NEGOTIATION_REJECTION no carrier bytes delivered");return;
            }
            MatCarrierPlayer player=session.player();
            String hash=player.submittedSha256(),independent=MatCarrierPlayer.hex(sink.digest.digest());
            if(result!=0 || player.state()!=MatCarrierPlayer.State.COMPLETED || !sink.closed ||
                player.writtenCycles()!=178 || sink.bytes!=10936320 || !MatRecording.VERIFIED_SHA256.equals(hash) || !hash.equals(independent))
                throw new IllegalStateException("Session failed: "+result+" "+player.state()+" "+player.error()+" "+hash);
            String json="{\"completed\":true,\"cycles\":178,\"bytes\":10936320,\"sha256\":\""+hash+"\",\"fakeHardware\":true}";
            Files.write(Paths.get(args[4]+".playback.json"),json.getBytes(StandardCharsets.UTF_8),StandardOpenOption.CREATE_NEW);
            System.out.println("PASS network -> shared MAT receiver -> production JNI -> bounded player -> fake IEC sink SHA256 "+hash);
        }
    }
}
