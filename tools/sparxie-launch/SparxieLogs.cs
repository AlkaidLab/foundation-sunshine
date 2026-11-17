using System.Text;

namespace Sparxie.Launch;

/// <summary>Sparxie writes its own Broker and SessionHost logs (logs\broker-*.log, host-*.log). On a failed attempt the lines written since it began are copied into the launcher log.</summary>
internal sealed class SparxieLogs
{
    public const int MaxLinesPerFile = 40;
    private readonly string? _dir;
    private readonly Dictionary<string, long> _offsets = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Sparxie's data root holds logs\ (the folder above app-*, as Sparxie resolves it).</summary>
    public static string? DirectoryFor(string brokerPath)
    {
        var dir = Path.GetDirectoryName(brokerPath);
        if (string.IsNullOrEmpty(dir)) return null;
        var name = Path.GetFileName(dir.TrimEnd('\\', '/'));
        if (name.StartsWith("app-", StringComparison.OrdinalIgnoreCase)) dir = Path.GetDirectoryName(dir.TrimEnd('\\', '/')) ?? dir;
        return Path.Combine(dir, "logs");
    }

    public SparxieLogs(string? dir)
    {
        _dir = dir;
        foreach (var file in Files()) _offsets[file.FullName] = file.Length;
    }

    private IEnumerable<FileInfo> Files()
    {
        if (_dir is null || !Directory.Exists(_dir)) return [];
        try
        {
            var info = new DirectoryInfo(_dir);
            return info.GetFiles("broker-*.log").Concat(info.GetFiles("host-*.log")).ToList();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return []; }
    }

    /// <summary>Framework chatter is dropped; Sparxie's own lines, warnings and errors are kept.</summary>
    public static bool Keep(string line)
    {
        if (line.Trim().Length == 0) return false;
        if (line.StartsWith("   at ", StringComparison.Ordinal)) return false;   // stack frames of a cancellation the launcher itself caused when it closed the stream
        if (line.StartsWith("System.OperationCanceledException", StringComparison.Ordinal)) return false;
        if (line.Contains("[Debug]", StringComparison.Ordinal) || line.Contains("[Trace]", StringComparison.Ordinal)) return false;
        if (line.Contains("[Information] Microsoft.", StringComparison.Ordinal) || line.Contains("[Information] Grpc.", StringComparison.Ordinal)) return false;
        return true;
    }

    public void DumpNew(RunLog log)
    {
        foreach (var file in Files())
        {
            try
            {
                var start = _offsets.TryGetValue(file.FullName, out var at) && at <= file.Length ? at : 0;
                if (file.Length <= start) continue;
                using var stream = new FileStream(file.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                stream.Seek(start, SeekOrigin.Begin);
                using var reader = new StreamReader(stream, new UTF8Encoding(false));
                var lines = new List<string>();
                string? line;
                while ((line = reader.ReadLine()) is not null) if (Keep(line)) lines.Add(line);
                if (lines.Count == 0) continue;
                log.Info($"sparxie {file.Name}: {lines.Count} new line(s){(lines.Count > MaxLinesPerFile ? $", the last {MaxLinesPerFile} follow" : "")}");
                foreach (var l in lines.Skip(Math.Max(0, lines.Count - MaxLinesPerFile))) log.Info($"sparxie {file.Name}: {(l.Length <= 400 ? l : l[..400] + "...")}");
                _offsets[file.FullName] = file.Length;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { log.Warn($"could not read {file.Name}: {ex.Message}"); }
        }
    }
}
