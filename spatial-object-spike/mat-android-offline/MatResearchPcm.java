package com.limelight.binding.audio;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;

/** Fixed-fixture research extraction, mono audition only. No Atmos renderer or XYZ interpretation. */
public final class MatResearchPcm {
    private static final byte[] FULL = hex("834143c2082f"), LFE = hex("834140f20b2f");
    private static final byte[] TABLE = hex("01d61f37cdf37cdf37cdf37cdf37cdf37cdf");
    private static byte[] hex(String s) {
        byte[] b = new byte[s.length()/2];
        for (int i=0;i<b.length;i++) b[i]=(byte)Integer.parseInt(s.substring(i*2,i*2+2),16);
        return b;
    }
    private static ArrayList<Integer> find(byte[] bytes, byte[] marker) {
        ArrayList<Integer> matches = new ArrayList<>();
        for (int i=0;i<=bytes.length-marker.length;i++) {
            int j=0; while (j<marker.length && bytes[i+j]==marker[j]) j++;
            if (j==marker.length) matches.add(i);
        }
        return matches;
    }
    public static byte[] load(InputStream input) throws IOException {
        byte[] raw = new byte[178*61440];
        int offset=0;
        while (offset<raw.length) {
            int n=input.read(raw,offset,raw.length-offset);
            if (n<=0) throw new IOException("Incomplete fixed MAT fixture");
            offset+=n;
        }
        if (input.read()!=-1) throw new IOException("Oversized fixed MAT fixture");
        return raw;
    }
    private static int sample(byte[] bytes, int offset) {
        return (short)(((bytes[offset]&255)<<8)|(bytes[offset+1]&255));
    }
    public static short[] render(byte[] raw) throws IOException {
        MatRecording.readVerified(new ByteArrayInputStream(raw));
        short[] stereo = new short[178*960*2];
        for (int cycle=0;cycle<178;cycle++) {
            int base=cycle*61440;
            int pd=(raw[base+6]&255)|((raw[base+7]&255)<<8);
            byte[] logical=new byte[pd];
            for (int i=0;i<pd;i++) logical[i]=raw[base+8+(i^1)];
            ArrayList<Integer> full=find(logical,FULL), low=find(logical,LFE), tables=find(logical,TABLE);
            boolean valid=full.size()==62 && low.size()==2 && tables.size()==2;
            // The original recording has exactly three bootstrap cycles without
            // the stable research layout. Keep their duration as PCM silence.
            if (cycle<3) {
                if (valid) throw new IOException("Unexpected bootstrap layout");
                continue;
            }
            if (!valid) throw new IOException("Unsupported research layout at cycle "+cycle);
            for (int p:full) if (p+FULL.length+960>pd) throw new IOException("Truncated fullband slot");
            for (int p:low) {
                if (p+LFE.length+240>pd) throw new IOException("Truncated LFE slot");
                for (int i=0;i<120;i++) if (sample(logical,p+LFE.length+i*2)!=0)
                    throw new IOException("Fixed audition requires silent LFE");
            }
            for (int p:tables) if (p+TABLE.length+90>pd) throw new IOException("Truncated object fields");
            for (int half=0;half<2;half++) for (int frame=0;frame<480;frame++) {
                int sum=0;
                for (int slot=0;slot<31;slot++) sum+=sample(logical,full.get(half*31+slot)+FULL.length+frame*2);
                int mixed=sum/4;
                if (mixed<Short.MIN_VALUE || mixed>Short.MAX_VALUE) throw new IOException("Audition mix would clip");
                int index=(cycle*960+half*480+frame)*2;
                stereo[index]=stereo[index+1]=(short)mixed;
            }
        }
        return stereo;
    }
    public static String sha256(short[] words) throws Exception {
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        for (short word:words) {digest.update((byte)word);digest.update((byte)(word>>>8));}
        return MatCarrierPlayer.hex(digest.digest());
    }
    public static int peak(short[] words) {
        int peak=0;
        for (short word:words) peak=Math.max(peak,Math.abs((int)word));
        return peak;
    }
    /** Audition gain only: preserve the independently checked extraction array. */
    public static short[] audition(short[] extracted) throws IOException {
        int peak=peak(extracted);
        if (peak==0) throw new IOException("Silent research extraction");
        short[] output=new short[extracted.length];
        for (int i=0;i<output.length;i++) output[i]=(short)(extracted[i]*8192/peak);
        return output;
    }
}
