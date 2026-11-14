using System.Globalization;
using System.Text;

namespace Sparxie.Launch;

/// <summary>One log file per run, a timestamp on every line, the last <see cref="Keep"/> runs kept.</summary>
internal sealed class RunLog : IDisposable
{
    public const int Keep = 20;
    public const string Prefix = "sparxie-launch-";
    public const string LogDirEnv = "SPARXIE_LAUNCH_LOG_DIR";

    private readonly object _gate = new();
    private readonly StreamWriter? _writer;
    private readonly Func<DateTime> _clock;

    public string? Path { get; }

    /// <summary>Put before every message (the attempt number).</summary>
    public string LinePrefix { get; set; } = "";

    private RunLog(StreamWriter? writer, string? path, Func<DateTime> clock)
    {
        _writer = writer;
        Path = path;
        _clock = clock;
    }

    /// <summary>
    /// The first of: the env override, the host's log folder (config\logs beside the install's tools\ folder), a Sunshine folder under
    /// LocalAppData, the temp folder. The first one that can be created and written wins.
    /// </summary>
    public static IEnumerable<string> CandidateDirectories(string exeDirectory, Func<string, string?> env)
    {
        var over = env(LogDirEnv);
        if (!string.IsNullOrWhiteSpace(over)) yield return over;
        var install = System.IO.Path.GetDirectoryName(exeDirectory.TrimEnd('\\', '/'));
        if (!string.IsNullOrEmpty(install) && string.Equals(System.IO.Path.GetFileName(exeDirectory.TrimEnd('\\', '/')), "tools", StringComparison.OrdinalIgnoreCase))
            yield return System.IO.Path.Combine(install, "config", "logs");
        var local = env("LOCALAPPDATA");
        if (!string.IsNullOrWhiteSpace(local)) yield return System.IO.Path.Combine(local, "Sunshine", "logs");
        yield return System.IO.Path.Combine(System.IO.Path.GetTempPath(), "Sunshine", "logs");
    }

    public static RunLog Open(IEnumerable<string> directories, Func<DateTime>? clock = null)
    {
        clock ??= () => DateTime.Now;
        foreach (var dir in directories)
        {
            try
            {
                Directory.CreateDirectory(dir);
                var stamp = clock().ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
                for (var n = 0; n < 50; n++)
                {
                    var name = Prefix + stamp + (n == 0 ? "" : "-" + n.ToString(CultureInfo.InvariantCulture)) + ".log";
                    var path = System.IO.Path.Combine(dir, name);
                    try
                    {
                        var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
                        var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };
                        Prune(dir, path);
                        return new RunLog(writer, path, clock);
                    }
                    catch (IOException) when (File.Exists(path)) { /* same second: next name */ }
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Security.SecurityException or NotSupportedException or ArgumentException)
            {
                // this folder is not usable; the next one is tried
            }
        }
        return new RunLog(null, null, clock);
    }

    /// <summary>Deletes the oldest logs of this launcher beyond <see cref="Keep"/>; the file just opened always stays.</summary>
    public static void Prune(string directory, string keepPath)
    {
        try
        {
            var files = new DirectoryInfo(directory).GetFiles(Prefix + "*.log")
                .OrderByDescending(f => f.Name, StringComparer.Ordinal)
                .ToList();
            foreach (var old in files.Skip(Keep))
            {
                if (string.Equals(old.FullName, keepPath, StringComparison.OrdinalIgnoreCase)) continue;
                try { old.Delete(); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }

    public void Info(string message) => Write("INFO ", message);
    public void Warn(string message) => Write("WARN ", message);
    public void Error(string message) => Write("ERROR", message);

    private void Write(string level, string message)
    {
        var line = string.Create(CultureInfo.InvariantCulture, $"{_clock():yyyy-MM-dd HH:mm:ss.fff} {level} {LinePrefix}{message.ReplaceLineEndings(" | ")}");
        lock (_gate)
        {
            try { _writer?.WriteLine(line); }
            catch (Exception ex) when (ex is IOException or ObjectDisposedException) { }
        }
    }

    public void Dispose()
    {
        lock (_gate) { _writer?.Dispose(); }
    }
}
