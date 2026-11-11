package com.sunshine.matmock;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import org.json.JSONArray;
import org.json.JSONObject;
import java.nio.ByteBuffer;
import java.security.MessageDigest;
import java.util.ArrayDeque;

/** Instrumented diagnostic only: real JNI/crypto/reassembly, generated input, mock output. */
public final class MatInstrumentation extends Instrumentation {
    static { System.loadLibrary("matjni"); }
    private static native int runNative(Output output, long epoch, int mode);
    private static native long[] fecStatsNative();
    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder();
        for (byte b : bytes) text.append(String.format("%02x", b & 255));
        return text.toString();
    }
    static final class CallbackException extends RuntimeException {}
    static final class Output {
        final ArrayDeque<byte[]> queue = new ArrayDeque<>();
        final MessageDigest expected, consumed;
        final long epoch, domain, caller = Thread.currentThread().getId();
        final boolean generated;
        final int mode;
        int cycles, ends, peak, offset, writes, zeroWrites, shortWrites;
        long consumedBytes;
        String sourceSha, outputSha;
        boolean completed, closed, foreignThread, pendingAtEnd;
        Output(long epoch, int mode) throws Exception {
            this(epoch, mode, 7, true);
        }
        Output(long epoch, int mode, long domain, boolean generated) throws Exception {
            this.epoch = epoch; this.mode = mode; this.domain = domain; this.generated = generated;
            expected = MessageDigest.getInstance("SHA-256");
            consumed = MessageDigest.getInstance("SHA-256");
        }
        public boolean onCycle(ByteBuffer borrowed, long e, long domain, long index, long pts) {
            foreignThread = Thread.currentThread().getId() != caller;
            if (mode == 5) throw new CallbackException();
            if (mode == 7) return false;
            check(foreignThread && !closed && !completed, "callback thread/lifecycle");
            check(e == epoch && domain == this.domain && index == cycles && pts == index * 960, "cycle identity");
            check(borrowed.isDirect() && borrowed.remaining() == 61440, "borrowed native buffer");
            if (queue.size() == 4) return false;
            byte[] copy = new byte[61440]; borrowed.get(copy);
            // Native storage is reused by the next cycle; only the owned copy may be queued.
            queue.add(copy); peak = Math.max(peak, queue.size());
            byte[] source = copy;
            if (generated) {
                source = new byte[61440];
                for (int j = 0; j < source.length; ++j) source[j] = (byte)(j * 7 + cycles);
                byte[] header = {0x72, (byte)0xf8, 0x1f, 0x4e, 0x16, 0, 2, 0};
                System.arraycopy(header, 0, source, 0, header.length);
            }
            expected.update(source); ++cycles;
            return queue.size() < 3 || drain(1);
        }
        boolean drain(int remaining) {
            // Fixed budget: short and zero writes cannot make EOS wait indefinitely.
            for (int budget = 0; budget < 256 && queue.size() > remaining; ++budget) {
                if (closed || mode == 3 || mode == 4) return false;
                ++writes;
                if (writes % 5 == 0) { ++zeroWrites; continue; }
                byte[] head = queue.peek(); int bytes = Math.min(4096, head.length - offset);
                if (bytes < head.length - offset) ++shortWrites;
                consumed.update(head, offset, bytes); consumedBytes += bytes; offset += bytes;
                if (offset == head.length) { queue.remove(); offset = 0; }
            }
            return queue.size() <= remaining;
        }
        public boolean onEnd(long count) {
            ++ends; pendingAtEnd = !queue.isEmpty();
            if (mode == 4) close(); // Cancellation while the verified END is being handled.
            completed = !closed && count == cycles && drain(0);
            return completed;
        }
        void close() { closed = true; queue.clear(); }
    }
    private Bundle arguments;
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); this.arguments = arguments; start(); }
    @Override public void onStart() {
        Bundle result = new Bundle(); JSONArray cases = new JSONArray();
        try {
            if (arguments != null && arguments.containsKey("sessionUrl")) {
                result.putString("proof", Session.run(arguments).toString());
                finish(Activity.RESULT_OK, result); return;
            }
            for (int mode = 0; mode < 8; ++mode) {
                Output output = new Output(123, mode); int code = -1; boolean threw = false;
                try { code = runNative(output, 123, mode); }
                catch (CallbackException expected) { threw = true; }
                if (mode == 0) verifySuccess(output, code);
                else if (mode == 2) check(code == 1 && output.cycles == 1 && output.ends == 0, "EOF is not EOS");
                else if (mode == 5) check(threw && output.cycles == 0, "original callback exception");
                else check(code < 0 && !output.completed, "failure must reject completion");
                if (mode == 1 || mode == 6) check(output.cycles == 0 && output.ends == 0, "unauthenticated callback");
                output.close();
                String[] names = {"copy-sha-drain", "tamper", "missing-end", "sink-error",
                    "cancel-at-end", "callback-exception", "stale-epoch", "callback-reject"};
                JSONObject item = new JSONObject().put("name", names[mode]).put("passed", true)
                    .put("cycles", output.cycles).put("endCallbacks", output.ends);
                if (mode == 0) item.put("sourceSha256", output.sourceSha).put("outputSha256", output.outputSha)
                    .put("consumedBytes", output.consumedBytes).put("peakQueueCycles", output.peak);
                cases.put(item);
            }
            Output fec = new Output(123, 0);
            try {
                verifySuccess(fec, runNative(fec, 123, 8));
                long[] stats = fecStatsNative();
                check(stats[0] > 0 && stats[0] == stats[1], "three data + one parity recovery");
                cases.put(new JSONObject().put("name", "fec-three-data-one-parity").put("passed", true)
                    .put("dropped", stats[0]).put("recovered", stats[1])
                    .put("sourceSha256", fec.sourceSha).put("outputSha256", fec.outputSha));
            } finally { fec.close(); }
            for (long epoch = 124; epoch <= 125; ++epoch) {
                Output output = new Output(epoch, 0);
                verifySuccess(output, runNative(output, epoch, 0)); output.close();
            }
            cases.put(new JSONObject().put("name", "fresh-session-reconnect").put("passed", true));
            JSONObject proof = new JSONObject().put("passed", true).put("cases", cases)
                .put("cyclesPerSuccessfulSession", 128).put("successfulSessions", 4)
                .put("source", "generated encrypted packets")
                .put("nativeWorkerThread", true).put("output", "mock").put("hardwareSink", false)
                .put("rtspSession", false).put("rtpFec", true).put("pid", android.os.Process.myPid());
            result.putString("proof", proof.toString());
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            result.putString("failure", android.util.Log.getStackTraceString(error));
            finish(Activity.RESULT_CANCELED, result);
        }
    }
    private static void verifySuccess(Output output, int code) {
        check(code == 2 && output.completed && output.ends == 1 && output.cycles == 128, "verified EOS");
        check(output.foreignThread && output.pendingAtEnd && output.queue.isEmpty(), "thread/END drain");
        check(output.peak == 3 && output.zeroWrites > 0 && output.shortWrites > 0, "bounded copy queue");
        check(output.consumedBytes == 128L * 61440, "output byte count");
        output.sourceSha = hex(output.expected.digest()); output.outputSha = hex(output.consumed.digest());
        check(output.sourceSha.equals(output.outputSha), "owned copy SHA256");
    }
}
