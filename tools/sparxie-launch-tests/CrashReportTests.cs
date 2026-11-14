using System.Text.Json;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace Sparxie.Launch;

internal static partial class Tests
{
    private static void CrashReports(string scratch)
    {
        DumpPrivacy_();
        // the signature is the host's: SHA-256 of "sunshine.exe+0x1234\nsunshine.exe+0x77"
        var frames = new List<CrashReport.Frame> { new("Sunshine.EXE", 0x1234), new("sunshine.exe", 0x77) };
        var expected = Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes("sunshine.exe+0x1234\nsunshine.exe+0x77")));
        Same(expected, CrashReport.Signature(frames), "crash: signature");
        var withRuntime = new List<CrashReport.Frame> { new("System.Private.CoreLib.dll", 5), new("sparxie-launch.exe", 0x500), new("sparxie-launch.exe", 0x600) };
        var without = withRuntime.Skip(1).ToList();
        Same(CrashReport.Signature(without), CrashReport.Signature(withRuntime), "crash: runtime frames on top do not count");
        var many = Enumerable.Range(0, 12).Select(i => new CrashReport.Frame("a.exe", (ulong)(0x100 + i))).ToList();
        var before = CrashReport.Signature(many);
        many[10] = new CrashReport.Frame("a.exe", 0xdead);
        Same(before, CrashReport.Signature(many), "crash: only the first eight frames count");

        Same("2023-11-14T22:13:20Z", CrashReport.IsoUtc(DateTimeOffset.FromUnixTimeSeconds(1700000000)), "crash: time");

        var meta = JsonDocument.Parse(CrashReport.RenderMeta("2026.1112.1.杂鱼", "Windows 11", "unhandled-dotnet", "dotnet:System.InvalidOperationException",
            DateTimeOffset.FromUnixTimeSeconds(1700000000), 42, [new("System.Private.CoreLib.dll", 1), new("sparxie-launch.exe", 0xabc)])).RootElement;
        Same(1, meta.GetProperty("v").GetInt32(), "crash meta: v");
        Same("sparxie-launch", meta.GetProperty("product").GetString(), "crash meta: product");
        Same("sparxie-launch.exe", meta.GetProperty("module").GetString(), "crash meta: first caller module");
        Same("0xabc", meta.GetProperty("module_offset").GetString(), "crash meta: offset");
        Same("unhandled-dotnet", meta.GetProperty("kind").GetString(), "crash meta: kind");
        Same(64, meta.GetProperty("signature").GetString()!.Length, "crash meta: signature length");
        Check(!meta.TryGetProperty("install_id", out _), "crash meta: no install id until upload");

        Check(CrashReport.ErrorsEnabled("") && CrashReport.ErrorsEnabled("telemetry_errors = on\n") && !CrashReport.ErrorsEnabled("telemetry_errors = off\n"), "crash consent: key");
        Check(!CrashReport.ErrorsEnabled("telemetry_consent = off\n") && CrashReport.ErrorsEnabled("telemetry_consent = off\ntelemetry_errors = on\n"), "crash consent: an old no stays no");
        Check(CrashReport.Settled(200) && CrashReport.Settled(400) && !CrashReport.Settled(429) && !CrashReport.Settled(503) && !CrashReport.Settled(0), "crash upload: what is settled");

        // folders: the override first, then the host's beside tools\
        var install = Path.Combine(scratch, "inst");
        var candidates = CrashReport.CandidateDirectories(Path.Combine(install, "tools"), n => n == CrashReport.DirEnv ? "X:\\o" : null).ToList();
        Same("X:\\o", candidates[0].Path, "crash dirs: override first");
        Check(candidates[1].HostUploads && candidates[1].Path == Path.Combine(install, "config", "crashes", "pending"), "crash dirs: the host's folder, which the host uploads");

        // a real exception becomes frames
        try { throw new InvalidOperationException("secret text must not leave"); }
        catch (Exception ex) { Check(CrashReport.FramesOf(ex).Count > 0 && CrashReport.FramesOf(ex).All(f => !f.Module.Contains('\\')), "crash frames: from a thrown exception, no paths"); }

        // identity is read only
        var cfg = Path.Combine(scratch, "cfg");
        Directory.CreateDirectory(Path.Combine(cfg, "telemetry"));
        Check(CrashReport.ReadInstallId(cfg) is null, "crash identity: absent");
        File.WriteAllText(Path.Combine(cfg, "telemetry", "install_id"), "11111111-2222-4333-8444-555555555555\n");
        Same("11111111-2222-4333-8444-555555555555", CrashReport.ReadInstallId(cfg), "crash identity: read");

        // upload against a local mock
        var dir = Path.Combine(scratch, "pending");
        Directory.CreateDirectory(dir);
        void Report(string name, bool dump)
        {
            File.WriteAllBytes(Path.Combine(dir, name + ".json"), CrashReport.RenderMeta("1", "w", "unhandled-dotnet", "dotnet:X", DateTimeOffset.UtcNow, 1, [new("sparxie-launch.exe", 0x10)]));
            if (dump) File.WriteAllText(Path.Combine(dir, name + ".dmp"), "MDMP-body");
            File.SetLastWriteTimeUtc(Path.Combine(dir, name + ".json"), DateTime.UtcNow.AddMinutes(-5));
        }
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        var status = 200;
        var bodies = new List<string>();
        var serve = Task.Run(async () =>
        {
            while (true)
            {
                TcpClient c;
                try { c = await listener.AcceptTcpClientAsync(); } catch (Exception) { return; }
                using (c)
                {
                    var s = c.GetStream();
                    var data = new List<byte>();
                    var buf = new byte[8192];
                    int headerEnd = -1, length = 0;
                    while (true)
                    {
                        var n = await s.ReadAsync(buf);
                        if (n <= 0) break;
                        data.AddRange(buf.AsSpan(0, n).ToArray());
                        var text = Encoding.Latin1.GetString(data.ToArray());
                        if (headerEnd < 0 && text.Contains("\r\n\r\n"))
                        {
                            headerEnd = text.IndexOf("\r\n\r\n", StringComparison.Ordinal) + 4;
                            var low = text[..headerEnd].ToLowerInvariant();
                            var at = low.IndexOf("content-length:", StringComparison.Ordinal);
                            if (at >= 0) length = int.Parse(low[(at + 15)..].Split('\r')[0].Trim());
                        }
                        if (headerEnd >= 0 && data.Count >= headerEnd + length) break;
                    }
                    if (headerEnd >= 0) lock (bodies) bodies.Add(Encoding.Latin1.GetString(data.ToArray())[headerEnd..]);
                    await s.WriteAsync(Encoding.ASCII.GetBytes($"HTTP/1.1 {status} X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
                }
            }
        });
        var endpoint = $"http://127.0.0.1:{port}";
        Report("a", true);
        Report("b", false);
        status = 503;
        Same(0, CrashReport.UploadPendingAsync(dir, endpoint, "11111111-2222-4333-8444-555555555555").GetAwaiter().GetResult(), "crash upload: 503 delivers nothing");
        Same(2, Directory.GetFiles(dir, "*.json").Length, "crash upload: 503 keeps the reports");
        Same(1, bodies.Count, "crash upload: 503 stops the pass after one try");
        status = 200;
        bodies.Clear();
        Same(2, CrashReport.UploadPendingAsync(dir, endpoint, "11111111-2222-4333-8444-555555555555").GetAwaiter().GetResult(), "crash upload: 2xx delivers both");
        Same(0, Directory.GetFiles(dir).Length, "crash upload: delivered reports are deleted with their dumps");
        lock (bodies)
        {
            Check(bodies.Count == 2 && bodies[0].Contains("name=\"meta\"") && bodies[0].Contains("name=\"dump\"") && bodies[0].Contains("MDMP-body"), "crash upload: meta and dump parts");
            Check(bodies[0].Contains("\"install_id\":\"11111111-2222-4333-8444-555555555555\"") && !bodies[1].Contains("name=\"dump\""), "crash upload: identity added, no dump part without a dump");
        }
        listener.Stop();
        serve.Wait(2000);

        // the pending folder keeps the newest twenty
        var cap = Path.Combine(scratch, "cap");
        Directory.CreateDirectory(cap);
        for (var i = 0; i < 24; i++)
        {
            var f = Path.Combine(cap, $"r{100 + i}.json");
            File.WriteAllText(f, "{}");
            File.SetLastWriteTimeUtc(f, DateTime.UtcNow.AddMinutes(-100 + i));
        }
        CrashReport.TrimPending(cap);
        Same(CrashReport.MaxPending, Directory.GetFiles(cap, "*.json").Length, "crash pending: capped at twenty");
        Check(!File.Exists(Path.Combine(cap, "r100.json")) && File.Exists(Path.Combine(cap, "r123.json")), "crash pending: the oldest went");
    }

    // ---- the privacy pass over a dump: the same vectors as the host's test_crash.cpp and the Panel's redact.rs ----
    private static byte[] Wide(string text) => Encoding.Unicode.GetBytes(text);

    private static int Find(byte[] data, byte[] needle)
    {
        for (var i = 0; i + needle.Length <= data.Length; i++)
        {
            var hit = true;
            for (var k = 0; k < needle.Length && hit; k++) hit = data[i + k] == needle[k];
            if (hit) return i;
        }
        return -1;
    }

    private static void DumpPrivacy_()
    {
        var data = Encoding.ASCII.GetBytes("MDMP\0\0C:\\Users\\alice\\AppData\\sunshine.exe\0\0\0/home/bob/.config\0\0/Users/carol Smith/x").Concat(Wide("C:\\Users\\dave\\Desktop\\sunshine.exe")).ToArray();
        var length = data.Length;
        Same(4, DumpPrivacy.Redact(data, []), "dump privacy: account names behind home markers");
        Same(length, data.Length, "dump privacy: same length");
        foreach (var secret in new[] { "alice", "bob", "carol", "Smith" }) Check(Find(data, Encoding.ASCII.GetBytes(secret)) < 0, "dump privacy: " + secret + " gone");
        Check(Find(data, Wide("dave")) < 0, "dump privacy: utf-16 account gone");
        Check(Find(data, Encoding.ASCII.GetBytes("\\Users\\xxxxx\\AppData\\sunshine.exe")) >= 0, "dump privacy: folders after the account stay");

        var cjk = Wide("C:\\Users\\").Concat(new byte[] { 0x0f, 0x5c, 0x0e, 0x66 }).Concat(Wide("\\Documents")).ToArray();
        DumpPrivacy.Redact(cjk, []);
        Check(Find(cjk, [0x0f, 0x5c]) < 0 && Find(cjk, Wide("\\Documents")) >= 0, "dump privacy: non-ASCII account name");

        var tokens = Encoding.ASCII.GetBytes("host SKYPC-01 and skypc-01; user Sky Hua; sunshine.exe SkyHua2").Concat(Wide("SKYPC-01")).Concat(Wide(" ")).Concat(Wide("skyhua")).ToArray();
        Same(4, DumpPrivacy.Redact(tokens, ["SkyPC-01", "SkyHua", "sun", "ab"]), "dump privacy: names as whole tokens");
        Check(Find(tokens, Encoding.ASCII.GetBytes("SKYPC-01")) < 0 && Find(tokens, Wide("SKYPC-01")) < 0 && Find(tokens, Wide("skyhua")) < 0, "dump privacy: known names gone");
        Check(Find(tokens, Encoding.ASCII.GetBytes("sunshine.exe")) >= 0 && Find(tokens, Encoding.ASCII.GetBytes("SkyHua2")) >= 0, "dump privacy: longer words are not hits");

        var clean = Encoding.ASCII.GetBytes("MDMP").Concat(Enumerable.Repeat((byte)0x41, 200)).Concat(Encoding.ASCII.GetBytes("sunshine.exe")).ToArray();
        var copy = (byte[])clean.Clone();
        Same(0, DumpPrivacy.Redact(clean, ["alice"]), "dump privacy: nothing private, nothing changed");
        Check(clean.SequenceEqual(copy), "dump privacy: bytes untouched");
        Check((CrashReport.DumpType & 0x40) == 0 && (CrashReport.DumpType & 0x2) == 0 && (CrashReport.DumpType & 0x4) == 0 && (CrashReport.DumpType & 0x8) != 0 && (CrashReport.DumpType & 0x80) != 0, "dump type: no indirect memory, no full memory, no handle data; stacks filtered, module paths filtered");
    }
}
