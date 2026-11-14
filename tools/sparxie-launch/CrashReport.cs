using System.Diagnostics;
using System.Net.Http.Headers;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Sparxie.Launch;

/// <summary>
/// Crash reports of the launcher (telemetry v1, <c>POST /v1/crash</c>): an exception nobody caught (<c>AppDomain.UnhandledException</c>, kind
/// "unhandled-dotnet", with a minidump written through dbghelp while the process still lives) and a faulted task nobody observed
/// (<c>TaskScheduler.UnobservedTaskException</c>, meta only). A frame is the assembly file and (metadata token &lt;&lt; 32 | IL offset), so the
/// offline symboliser can name the method from the unstripped build; never an exception message (it may carry free text or a path).
/// The meta follows the host's (<c>src/crash/crash_core.cpp</c>): same signature, same fields.
///
/// Reports go to the first writable of SUNSHINE_CRASH_DIR, the host's <c>config\crashes\pending</c> (the host uploads those), a folder under
/// LocalAppData (this program uploads those itself, a pass about 20 s after the start, never waiting for it). Nothing is written or sent
/// when error reports are off (<c>telemetry_errors</c> of the host's sunshine.conf, default on).
/// </summary>
internal static class CrashReport
{
    public const string DirEnv = "SUNSHINE_CRASH_DIR";
    public const string TestEnv = "SUNSHINE_TEST_CRASH";
    public const string EndpointEnv = "SUNSHINE_TELEMETRY_ENDPOINT";
    public const string DelayEnv = "SUNSHINE_CRASH_UPLOAD_DELAY_S";
    public const string DefaultEndpoint = "https://api.alkaidlab.com";
    public const int MaxFrames = 64;
    public const int SignatureFrames = 8;
    public const int MaxPending = 20;
    public const long MaxDumpBytes = 20L * 1024 * 1024;
    private const int MaxReportsPerRun = 3;

    internal readonly record struct Frame(string Module, ulong Offset);

    private static readonly object Gate = new();
    private static readonly List<string> Seen = [];
    private static string? _pending;       // where reports are written
    private static string? _hostConfig;    // <install>\config when this program sits in <install>\tools
    private static string _version = "";
    private static string _osVersion = "";
    private static DateTime _started = DateTime.UtcNow;
    private static int _reports;

    // ---- pure parts (unit tested) ----------------------------------------------------------------------------------------------------

    /// <summary>Runtime and system modules whose frames say nothing about the caller.</summary>
    internal static bool IsRuntimeModule(string module) =>
        module.StartsWith("System.", StringComparison.OrdinalIgnoreCase) || module.StartsWith("Microsoft.", StringComparison.OrdinalIgnoreCase) ||
        module.Equals("netstandard.dll", StringComparison.OrdinalIgnoreCase) || module.Equals("mscorlib.dll", StringComparison.OrdinalIgnoreCase) ||
        module.Equals("ntdll.dll", StringComparison.OrdinalIgnoreCase) || module.Equals("kernelbase.dll", StringComparison.OrdinalIgnoreCase) ||
        module.Equals("kernel32.dll", StringComparison.OrdinalIgnoreCase) || module.Equals("coreclr.dll", StringComparison.OrdinalIgnoreCase);

    /// <summary>Index of the first frame that is not a runtime frame (0 when all are).</summary>
    internal static int FirstCallerFrame(IReadOnlyList<Frame> frames)
    {
        for (var i = 0; i < frames.Count; i++) if (!IsRuntimeModule(frames[i].Module)) return i;
        return 0;
    }

    /// <summary>SHA-256 over the first 8 frames from the first caller frame on, each "module(lower)+0xoffset(lower hex)" joined by newline; 64 hex digits.</summary>
    internal static string Signature(IReadOnlyList<Frame> frames)
    {
        var text = string.Join('\n', frames.Skip(FirstCallerFrame(frames)).Take(SignatureFrames).Select(f => $"{f.Module.ToLowerInvariant()}+0x{f.Offset:x}"));
        return Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
    }

    internal static string IsoUtc(DateTimeOffset time) => time.UtcDateTime.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>The meta JSON of the contract.</summary>
    internal static byte[] RenderMeta(string version, string osVersion, string kind, string code, DateTimeOffset time, long uptimeSeconds, IReadOnlyList<Frame> allFrames, string? installId = null)
    {
        var frames = allFrames.Count > MaxFrames ? allFrames.Take(MaxFrames).ToList() : allFrames;
        var caller = frames.Count > 0 ? frames[FirstCallerFrame(frames)] : new Frame("", 0);
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream))
        {
            w.WriteStartObject();
            w.WriteNumber("v", 1);
            w.WriteString("product", "sparxie-launch");
            if (!string.IsNullOrEmpty(installId)) w.WriteString("install_id", installId);
            w.WriteString("version", version);
            w.WriteString("os_version", osVersion);
            w.WriteString("time", IsoUtc(time));
            w.WriteNumber("uptime_s", uptimeSeconds);
            w.WriteString("exception_code", code);
            w.WriteString("module", caller.Module);
            w.WriteString("module_offset", $"0x{caller.Offset:x}");
            w.WriteStartArray("frames");
            foreach (var f in frames)
            {
                w.WriteStartObject();
                w.WriteString("module", f.Module);
                w.WriteString("offset", $"0x{f.Offset:x}");
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteString("signature", Signature(frames));
            w.WriteString("kind", kind);
            w.WriteEndObject();
        }
        return stream.ToArray();
    }

    /// <summary>The value of a key in a sunshine.conf text (the last one wins), empty when absent.</summary>
    internal static string ConfValue(string text, string key)
    {
        var found = "";
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] == '#') continue;
            var eq = line.IndexOf('=');
            if (eq < 0 || line[..eq].Trim() != key) continue;
            var v = line[(eq + 1)..].Trim();
            if (v.Length >= 2 && v[0] == '"' && v[^1] == '"') v = v[1..^1];
            found = v;
        }
        return found;
    }

    /// <summary>Error reports may leave the computer unless turned off: <c>telemetry_errors</c> decides; without it an old <c>telemetry_consent = off</c> still means off; else on.</summary>
    internal static bool ErrorsEnabled(string conf)
    {
        static bool Off(string v) => v.Equals("off", StringComparison.OrdinalIgnoreCase) || v.Equals("false", StringComparison.OrdinalIgnoreCase) || v == "0";
        var own = ConfValue(conf, "telemetry_errors");
        if (own.Length > 0) return !Off(own);
        return ConfValue(conf, "telemetry_consent") != "off";
    }

    /// <summary>The one-line UUID the host keeps in <c>config\telemetry\install_id</c>; null when absent or malformed (the host makes it when consent allows).</summary>
    internal static string? ReadInstallId(string configDir)
    {
        try
        {
            var line = File.ReadLines(Path.Combine(configDir, "telemetry", "install_id")).FirstOrDefault()?.Trim();
            return line is { Length: 36 } && Guid.TryParseExact(line, "D", out _) ? line : null;
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    internal static string Endpoint(string conf, Func<string, string?> env)
    {
        var fromEnv = env(EndpointEnv);
        if (!string.IsNullOrWhiteSpace(fromEnv)) return fromEnv;
        var fromConf = ConfValue(conf, "telemetry_endpoint");
        return fromConf.Length > 0 ? fromConf : DefaultEndpoint;
    }

    /// <summary>The host's config folder when this program sits in <c>&lt;install&gt;\tools</c>.</summary>
    internal static string? HostConfigDirectory(string exeDirectory)
    {
        var dir = exeDirectory.TrimEnd('\\', '/');
        var install = Path.GetDirectoryName(dir);
        return !string.IsNullOrEmpty(install) && string.Equals(Path.GetFileName(dir), "tools", StringComparison.OrdinalIgnoreCase) ? Path.Combine(install, "config") : null;
    }

    /// <summary>Folders reports may be written to, first choice first; the bool says whether the host uploads from it (so this program does not).</summary>
    internal static IEnumerable<(string Path, bool HostUploads)> CandidateDirectories(string exeDirectory, Func<string, string?> env)
    {
        var over = env(DirEnv);
        if (!string.IsNullOrWhiteSpace(over)) yield return (over, false);
        var config = HostConfigDirectory(exeDirectory);
        if (config is not null) yield return (Path.Combine(config, "crashes", "pending"), true);
        var local = env("LOCALAPPDATA");
        if (!string.IsNullOrWhiteSpace(local)) yield return (Path.Combine(local, "Sunshine", "crashes", "pending"), false);
        yield return (Path.Combine(Path.GetTempPath(), "Sunshine", "crashes", "pending"), false);
    }

    /// <summary>"unix seconds-sparxie-launch-kind-p pid hex", file-safe.</summary>
    internal static string Stem(long unixSeconds, string kind, int pid) => $"{unixSeconds}-sparxie-launch-{new string(kind.Select(c => char.IsAsciiLetterOrDigit(c) || c == '-' ? c : '_').ToArray())}-p{pid:x8}";

    /// <summary>Frames of an exception: the assembly file and (metadata token &lt;&lt; 32 | IL offset), innermost exception first (the root cause).</summary>
    internal static List<Frame> FramesOf(Exception exception)
    {
        while (exception.InnerException is { } inner) exception = inner;
        var frames = new List<Frame>();
        var entry = System.Reflection.Assembly.GetEntryAssembly();
        foreach (var frame in new StackTrace(exception, false).GetFrames())
        {
            var method = frame.GetMethod();
            if (method is null) { frames.Add(new Frame("unknown", 0)); continue; }
            var assembly = method.Module.Assembly;
            var name = assembly.GetName().Name ?? "unknown";
            var file = ReferenceEquals(assembly, entry) ? name + ".exe" : name + ".dll";
            var il = frame.GetILOffset();
            frames.Add(new Frame(file, ((ulong)(uint)method.MetadataToken << 32) | (uint)Math.Max(0, il)));
            if (frames.Count >= MaxFrames) break;
        }
        return frames;
    }

    /// <summary>Keeps the newest <paramref name="keep"/> reports of a folder, drops the rest and what is older than 30 days; a stray .dmp goes.</summary>
    internal static void TrimPending(string dir, int keep = MaxPending)
    {
        try
        {
            var metas = new DirectoryInfo(dir).GetFiles("*.json").OrderBy(f => f.LastWriteTimeUtc).ThenBy(f => f.Name).ToList();
            foreach (var dump in new DirectoryInfo(dir).GetFiles("*.dmp"))
                if (!File.Exists(Path.ChangeExtension(dump.FullName, ".json"))) dump.Delete();
            void Drop(FileInfo f) { f.Delete(); var d = Path.ChangeExtension(f.FullName, ".dmp"); if (File.Exists(d)) File.Delete(d); }
            foreach (var old in metas.Where(f => DateTime.UtcNow - f.LastWriteTimeUtc > TimeSpan.FromDays(30)).ToList()) { Drop(old); metas.Remove(old); }
            while (metas.Count > keep) { Drop(metas[0]); metas.RemoveAt(0); }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    /// <summary>A report the server will not take later either (2xx, or a 4xx other than 408/429).</summary>
    internal static bool Settled(int status) => status is >= 200 and < 300 || (status is >= 400 and < 500 && status is not (408 or 429));

    // ---- install, record, upload ------------------------------------------------------------------------------------------------------

    /// <summary>Hooks the handlers. <paramref name="exeDirectory"/> is where the launcher runs from.</summary>
    public static void Install(string exeDirectory, Func<string, string?> env)
    {
        lock (Gate)
        {
            if (_pending is not null) return;
            _started = DateTime.UtcNow;
            _version = typeof(CrashReport).Assembly.GetName().Version?.ToString() ?? "";
            _osVersion = $"Windows {Environment.OSVersion.Version}";
            _hostConfig = HostConfigDirectory(exeDirectory);
            foreach (var (dir, _) in CandidateDirectories(exeDirectory, env))
            {
                try
                {
                    Directory.CreateDirectory(dir);
                    var probe = Path.Combine(dir, $".w{Environment.ProcessId:x}");
                    File.WriteAllText(probe, "");
                    File.Delete(probe);
                    _pending = dir;
                    break;
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }
        if (_pending is null) return;
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Record("unhandled-dotnet", e.ExceptionObject as Exception, withDump: true);
        TaskScheduler.UnobservedTaskException += (_, e) => Record("unhandled-dotnet", e.Exception.Flatten(), withDump: false);
    }

    private static bool Enabled()
    {
        try { return ErrorsEnabled(_hostConfig is null ? "" : File.ReadAllText(Path.Combine(_hostConfig, "sunshine.conf"))); }
        catch (IOException) { return true; }
        catch (UnauthorizedAccessException) { return true; }
    }

    /// <summary>Writes one report; never throws (it runs while the process is going down).</summary>
    internal static void Record(string kind, Exception? exception, bool withDump)
    {
        try
        {
            var dir = _pending;
            if (dir is null || exception is null || !Enabled()) return;
            var frames = FramesOf(exception);
            var root = exception;
            while (root.InnerException is { } inner) root = inner;
            var code = "dotnet:" + (root.GetType().FullName ?? "Exception");
            if (code.Length > 60) code = code[..60];
            var signature = Signature(frames);
            lock (Gate)
            {
                if (_reports >= MaxReportsPerRun || Seen.Contains(signature)) return;
                _reports++;
                Seen.Add(signature);
            }
            var now = DateTimeOffset.UtcNow;
            var stem = Stem(now.ToUnixTimeSeconds(), kind, Environment.ProcessId);
            if (withDump) TryDump(Path.Combine(dir, stem + ".dmp"));
            var json = RenderMeta(_version, _osVersion, kind, code, now, (long)(DateTime.UtcNow - _started).TotalSeconds, frames);
            var temp = Path.Combine(dir, stem + ".json.tmp");
            File.WriteAllBytes(temp, json);
            File.Move(temp, Path.Combine(dir, stem + ".json"), true);
            TrimPending(dir);
        }
        catch (Exception) { /* the report is best effort */ }
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct MiniDumpException
    {
        public uint ThreadId;
        public IntPtr ExceptionPointers;
        public int ClientPointers;
    }

    [DllImport("dbghelp.dll", SetLastError = true)]
    private static extern bool MiniDumpWriteDump(IntPtr process, uint processId, Microsoft.Win32.SafeHandles.SafeFileHandle file, int dumpType, IntPtr exception, IntPtr user, IntPtr callback);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    // The smallest dump that still names the faulting place (MINIDUMP_TYPE, Microsoft Learn): Normal has no heap, no data segments and no handle data;
    // FilterMemory keeps only the pointer-sized values of each stack (a string or a token that lived on a stack is blanked); FilterModulePaths removes the
    // account name from module paths; WithoutOptionalData and WithoutAuxiliaryState drop what is not essential. WithIndirectlyReferencedMemory (the heap
    // behind each stack pointer) is NOT used.
    private const int DumpNormal = 0, DumpThreadInfo = 0x1000, DumpUnloaded = 0x20, DumpFilterMemory = 0x8, DumpFilterModulePaths = 0x80, DumpWithoutOptionalData = 0x400, DumpWithoutAuxiliaryState = 0x4000;
    internal const int DumpType = DumpNormal | DumpThreadInfo | DumpUnloaded | DumpFilterMemory | DumpFilterModulePaths | DumpWithoutOptionalData | DumpWithoutAuxiliaryState;

    /// <summary>A minidump of this process (threads, stacks reduced to pointers; no heap, no data, no full memory), dropped when over 20 MB.</summary>
    private static void TryDump(string path)
    {
        try
        {
            using var file = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None);
            var pointers = Marshal.GetExceptionPointers();
            var info = IntPtr.Zero;
            if (pointers != IntPtr.Zero)
            {
                info = Marshal.AllocHGlobal(Marshal.SizeOf<MiniDumpException>());
                Marshal.StructureToPtr(new MiniDumpException { ThreadId = GetCurrentThreadId(), ExceptionPointers = pointers, ClientPointers = 0 }, info, false);
            }
            using var process = Process.GetCurrentProcess();
            var ok = MiniDumpWriteDump(process.Handle, (uint)Environment.ProcessId, file.SafeFileHandle, DumpType, info, IntPtr.Zero, IntPtr.Zero);
            if (info != IntPtr.Zero) Marshal.FreeHGlobal(info);
            var keep = ok && file.Length <= MaxDumpBytes;
            file.Close();
            if (!keep) File.Delete(path);
        }
        catch (Exception) { try { File.Delete(path); } catch (IOException) { } }
    }

    /// <summary>One pass over a folder: POST every report, delete what was delivered or rejected for good; returns how many.</summary>
    internal static async Task<int> UploadPendingAsync(string dir, string endpoint, string installId, HttpClient? client = null, CancellationToken cancel = default)
    {
        TrimPending(dir);
        var own = $"-p{Environment.ProcessId:x8}.";
        var url = endpoint.TrimEnd('/') + "/v1/crash";
        var done = 0;
        using var ours = client is null ? new HttpClient { Timeout = TimeSpan.FromSeconds(120) } : null;
        client ??= ours!;
        IEnumerable<FileInfo> reports;
        try { reports = new DirectoryInfo(dir).GetFiles("*.json").OrderBy(f => f.LastWriteTimeUtc).ThenBy(f => f.Name).ToList(); }
        catch (IOException) { return 0; }
        foreach (var report in reports)
        {
            if (cancel.IsCancellationRequested) break;
            if (report.Name.Contains(own, StringComparison.Ordinal) && DateTime.UtcNow - report.LastWriteTimeUtc < TimeSpan.FromSeconds(10)) continue;
            var dump = Path.ChangeExtension(report.FullName, ".dmp");
            try
            {
                using var meta = JsonDocument.Parse(await File.ReadAllBytesAsync(report.FullName, cancel).ConfigureAwait(false));
                if (meta.RootElement.ValueKind != JsonValueKind.Object) throw new JsonException("not an object");
                var text = new Dictionary<string, JsonElement>();
                foreach (var p in meta.RootElement.EnumerateObject()) text[p.Name] = p.Value.Clone();
                using var buffer = new MemoryStream();
                using (var w = new Utf8JsonWriter(buffer))
                {
                    w.WriteStartObject();
                    foreach (var (k, v) in text) { w.WritePropertyName(k); v.WriteTo(w); }
                    if (!text.ContainsKey("install_id") && installId.Length > 0) w.WriteString("install_id", installId);
                    if (!text.ContainsKey("v")) w.WriteNumber("v", 1);
                    w.WriteEndObject();
                }
                using var form = new MultipartFormDataContent();
                var metaPart = new ByteArrayContent(buffer.ToArray());
                metaPart.Headers.ContentType = new MediaTypeHeaderValue("application/json");
                metaPart.Headers.ContentDisposition = new ContentDispositionHeaderValue("form-data") { Name = "\"meta\"" };
                form.Add(metaPart);
                if (File.Exists(dump) && new FileInfo(dump).Length <= MaxDumpBytes)
                {
                    var dumpBytes = await File.ReadAllBytesAsync(dump, cancel).ConfigureAwait(false);
                    // the dump type keeps heap and data out; this blanks the account and computer names that a path or a stack word still carries
                    DumpPrivacy.Redact(dumpBytes, DumpPrivacy.PrivateNames());
                    var dumpPart = new ByteArrayContent(dumpBytes);
                    dumpPart.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
                    dumpPart.Headers.ContentDisposition = new ContentDispositionHeaderValue("form-data") { Name = "\"dump\"", FileName = "\"crash.dmp\"" };
                    form.Add(dumpPart);
                }
                int status;
                try
                {
                    using var response = await client.PostAsync(url, form, cancel).ConfigureAwait(false);
                    status = (int)response.StatusCode;
                }
                catch (HttpRequestException) { status = 0; }
                catch (TaskCanceledException) { status = 0; }
                // 429 (a per-install limit) and 503 (storage failed, nothing counted): keep everything and stop this pass
                if (status is 429 or 503) break;
                if (!Settled(status)) continue;
            }
            catch (JsonException) { /* unreadable: it will never upload, so it goes */ }
            catch (IOException) { continue; }
            try { report.Delete(); if (File.Exists(dump)) File.Delete(dump); } catch (IOException) { }
            done++;
        }
        return done;
    }

    /// <summary>A pass about 20 s after the start (SUNSHINE_CRASH_UPLOAD_DELAY_S), only for the folders the host does not upload from; never waited for.</summary>
    public static void StartUploader(string exeDirectory, Func<string, string?> env)
    {
        if (_pending is null) return;
        var hostUploads = CandidateDirectories(exeDirectory, env).Any(c => c.HostUploads && c.Path == _pending);
        if (hostUploads) return;
        var dir = _pending;
        _ = Task.Run(async () =>
        {
            try
            {
                var delay = int.TryParse(env(DelayEnv), out var s) ? s : 20;
                await Task.Delay(TimeSpan.FromSeconds(Math.Max(0, delay))).ConfigureAwait(false);
                if (!Enabled() || _hostConfig is null) return;
                var id = ReadInstallId(_hostConfig);
                if (id is null) return;
                var conf = File.Exists(Path.Combine(_hostConfig, "sunshine.conf")) ? File.ReadAllText(Path.Combine(_hostConfig, "sunshine.conf")) : "";
                await UploadPendingAsync(dir, Endpoint(conf, env), id).ConfigureAwait(false);
            }
            catch (Exception) { /* best effort */ }
        });
    }

    /// <summary>SUNSHINE_TEST_CRASH=throw|task|upload-only: fail on purpose (tests only; never set in a real run).</summary>
    public static void RunTestSwitch(string exeDirectory, Func<string, string?> env)
    {
        switch (env(TestEnv))
        {
            case "throw":
                // an exception on a thread nobody guards: the runtime ends the process after UnhandledException
                var thread = new Thread(() => throw new InvalidOperationException("SUNSHINE_TEST_CRASH throw"));
                thread.Start();
                thread.Join();
                break;
            case "task":
                {
                    // a faulted task nobody looks at, found by the finalizer: UnobservedTaskException, the process lives
                    Task.Run(() => throw new InvalidOperationException("SUNSHINE_TEST_CRASH task"));
                    Thread.Sleep(300);
                    GC.Collect();
                    GC.WaitForPendingFinalizers();
                    GC.Collect();
                    Thread.Sleep(300);
                    Environment.Exit(0);
                    break;
                }
            case "upload-only":
                {
                    if (_pending is not null && Enabled() && _hostConfig is not null && ReadInstallId(_hostConfig) is { } id)
                    {
                        var conf = File.Exists(Path.Combine(_hostConfig, "sunshine.conf")) ? File.ReadAllText(Path.Combine(_hostConfig, "sunshine.conf")) : "";
                        UploadPendingAsync(_pending, Endpoint(conf, env), id).GetAwaiter().GetResult();
                    }
                    Environment.Exit(0);
                    break;
                }
        }
    }
}
