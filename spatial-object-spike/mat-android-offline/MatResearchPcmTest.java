package com.limelight.binding.audio;

import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

/** Byte-for-byte comparison with the existing Python research extractor. */
public final class MatResearchPcmTest {
    public static void main(String[] args) throws Exception {
        byte[] source=Files.readAllBytes(Paths.get(args[0]));
        short[] pcm=MatResearchPcm.render(source);
        byte[] bytes=new byte[pcm.length*2];
        for (int i=0;i<pcm.length;i++) {bytes[i*2]=(byte)pcm[i];bytes[i*2+1]=(byte)(pcm[i]>>>8);}
        if (!Arrays.equals(bytes,Files.readAllBytes(Paths.get(args[1])))) throw new AssertionError("PCM differs from Python research oracle");
        if (pcm.length!=178*960*2) throw new AssertionError("Audition duration changed");
        for (int i=0;i<3*960*2;i++) if (pcm[i]!=0) throw new AssertionError("Bootstrap duration must be silence");
        short[] snapshot=pcm.clone(), audition=MatResearchPcm.audition(pcm);
        if (!Arrays.equals(snapshot,pcm) || audition.length!=pcm.length || MatResearchPcm.peak(audition)!=8192)
            throw new AssertionError("Audition gain mutated extraction, duration or peak limit");
        short[] extremes=MatResearchPcm.audition(new short[]{Short.MIN_VALUE,0,Short.MAX_VALUE});
        if (extremes[0]!=-8192 || extremes[1]!=0 || extremes[2]>8192) throw new AssertionError("Audition gain overflow");
        try {MatResearchPcm.audition(new short[2]);throw new AssertionError("Silent audition accepted");}
        catch (java.io.IOException expected) { /* no silent success */ }
        source[100]^=1;
        try {MatResearchPcm.render(source);throw new AssertionError("Changed fixture accepted");}
        catch (java.io.IOException expected) { /* fixed SHA gate */ }
        System.out.println("PASS fixed MAT -> research PCM: "+(pcm.length/2)+" frames; sha256="+MatResearchPcm.sha256(pcm));
    }
}
