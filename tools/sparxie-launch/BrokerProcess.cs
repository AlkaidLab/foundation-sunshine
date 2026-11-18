using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

namespace Sparxie.Launch;

/// <summary>Finding and starting Sparxie.Broker.exe the way Sparxie's own GUI does: random pipe name, parent pid binding, elevated.</summary>
internal static class BrokerProcess
{
    public const string PipeOption = "--pipe-name=";
    public const string ParentOption = "--parent-pid=";
    public const int MaxPipeNameLength = 128;

    public static string NewPipeName() => "sparxie-" + Guid.NewGuid().ToString("N");

    public static bool IsValidPipeName(string? name)
    {
        if (string.IsNullOrWhiteSpace(name) || name.Length > MaxPipeNameLength) return false;
        foreach (var ch in name)
            if (!(char.IsAsciiLetterOrDigit(ch) || ch is '-' or '_' or '.')) return false;
        return true;
    }

    /// <summary>The Broker accepts exactly these two arguments, in this order.</summary>
    public static string Arguments(string pipeName, int parentPid)
    {
        if (!IsValidPipeName(pipeName)) throw new ArgumentException("the Broker pipe name is not valid", nameof(pipeName));
        if (parentPid <= 0) throw new ArgumentOutOfRangeException(nameof(parentPid));
        return PipeOption + pipeName + " " + ParentOption + parentPid;
    }

    /// <summary>Sparxie.Broker.exe beside Sparxie.exe, else in the newest app-* folder that has one.</summary>
    public static string? Locate(string sparxiePath)
    {
        var root = Path.GetDirectoryName(sparxiePath) ?? ".";
        var beside = Path.Combine(root, "Sparxie.Broker.exe");
        if (File.Exists(beside)) return beside;
        if (!Directory.Exists(root)) return null;
        return Directory.EnumerateDirectories(root, "app-*", SearchOption.TopDirectoryOnly)
            .Select(d => Path.Combine(d, "Sparxie.Broker.exe"))
            .Where(File.Exists)
            .OrderByDescending(p => File.GetLastWriteTimeUtc(p))
            .FirstOrDefault();
    }

    public static bool IsElevated()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    /// <summary>
    /// An elevated launcher (Sunshine starts the app elevated) creates the Broker directly: no UAC prompt, and the Broker is our child, so it
    /// shares the app's process group. A launcher that is not elevated asks Windows to elevate it (the same "runas" Sparxie's GUI uses).
    /// </summary>
    public static Process Start(string brokerPath, string pipeName, RunLog log, bool elevated)
    {
        var args = Arguments(pipeName, Environment.ProcessId);
        var dir = Path.GetDirectoryName(brokerPath) ?? ".";
        ProcessStartInfo info;
        if (elevated)
        {
            info = new ProcessStartInfo(brokerPath, args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = dir,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
        }
        else
        {
            info = new ProcessStartInfo(brokerPath, args)
            {
                UseShellExecute = true,
                Verb = "runas",
                WindowStyle = ProcessWindowStyle.Hidden,
                WorkingDirectory = dir,
            };
        }

        Process process;
        try
        {
            process = Process.Start(info) ?? throw new InvalidOperationException("Process.Start returned nothing");
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1223)
        {
            throw new InvalidOperationException("the administrator prompt for the Broker was cancelled");
        }
        catch (Win32Exception ex)
        {
            throw new InvalidOperationException($"Windows could not start the Broker (error {ex.NativeErrorCode}: {ex.Message})");
        }

        if (elevated)
        {
            Drain(process.StandardOutput, "broker stdout", log);
            Drain(process.StandardError, "broker stderr", log);
        }
        return process;
    }

    /// <summary>
    /// Keeps the Broker's console pipes empty so it never blocks. Its routine "info:" chatter is dropped; warnings, failures and plain
    /// text (such as an argument error) go to the log, up to 60 lines.
    /// </summary>
    private static void Drain(StreamReader reader, string label, RunLog log)
    {
        _ = Task.Run(async () =>
        {
            var shown = 0;
            var show = true;
            try
            {
                string? line;
                while ((line = await reader.ReadLineAsync().ConfigureAwait(false)) is not null)
                {
                    show = ShowsLine(line, show);
                    if (!show || line.Trim().Length == 0) continue;
                    if (shown < 60) log.Info($"{label}: {Truncate(line)}");
                    else if (shown == 60) log.Info($"{label}: more output is not logged");
                    shown++;
                }
            }
            catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException) { }
        });
    }

    /// <summary>Console logger lines start with a level ("info:", "warn:", ...) and continue on indented lines; returns whether this line is worth keeping.</summary>
    public static bool ShowsLine(string line, bool previous)
    {
        if (line.StartsWith("trce:", StringComparison.Ordinal) || line.StartsWith("dbug:", StringComparison.Ordinal) || line.StartsWith("info:", StringComparison.Ordinal)) return false;
        if (line.StartsWith("warn:", StringComparison.Ordinal) || line.StartsWith("fail:", StringComparison.Ordinal) || line.StartsWith("crit:", StringComparison.Ordinal)) return true;
        return line.Length > 0 && char.IsWhiteSpace(line[0]) ? previous : true;
    }

    private static string Truncate(string line) => line.Length <= 300 ? line : line[..300] + "...";

    /// <summary>How a running process of the game's name relates to the game executable we were asked to start.</summary>
    internal enum ImageMatch
    {
        Same,      // the image is exactly this executable
        Other,     // a program of the same name from another folder
        Unknown,   // the image path could not be read (a protected process, another bitness): the name is all there is
    }

    /// <summary>
    /// An unreadable path counts as the game: the cost of starting a second copy of a game that runs (an anti-cheat conflict, a Broker that refuses) is far
    /// higher than the cost of following a look-alike process, and the host itself decides "the game is running" by name alone.
    /// </summary>
    internal static ImageMatch Classify(string? readPath, string exePath)
    {
        if (string.IsNullOrWhiteSpace(readPath)) return ImageMatch.Unknown;
        try
        {
            return string.Equals(Path.GetFullPath(readPath), Path.GetFullPath(exePath), StringComparison.OrdinalIgnoreCase) ? ImageMatch.Same : ImageMatch.Other;
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException) { return ImageMatch.Unknown; }
    }

    internal static bool IsTheGame(ImageMatch match) => match is ImageMatch.Same or ImageMatch.Unknown;

    /// <summary>The processes that are this game executable (the Broker refuses to start a game that already runs). Same name with an unreadable image counts, see <see cref="Classify"/>.</summary>
    public static List<int> FindGameProcesses(string exePath)
    {
        var found = new List<int>();
        var name = Path.GetFileNameWithoutExtension(exePath);
        foreach (var p in Process.GetProcessesByName(name))
        {
            try
            {
                if (IsTheGame(Classify(ReadImagePath(p), exePath))) found.Add(p.Id);
            }
            finally { p.Dispose(); }
        }
        return found;
    }

    /// <summary>Everything we can see about the game's name and the Sparxie session hosts, for the log of an attempt that failed: what blocked the session is then in the file.</summary>
    public static IEnumerable<string> DescribeCandidates(string exePath)
    {
        var name = Path.GetFileNameWithoutExtension(exePath);
        foreach (var n in new[] { name, "Sparxie.SessionHost", "Sparxie.Broker" })
        {
            foreach (var p in Process.GetProcessesByName(n))
            {
                try
                {
                    var path = ReadImagePath(p);
                    var kind = n == name ? Classify(path, exePath).ToString() : "-";
                    yield return $"process {p.ProcessName} pid={p.Id} session={p.SessionId} image={(path ?? "unreadable")} match={kind}";
                }
                finally { p.Dispose(); }
            }
        }
    }

    private static string? ReadImagePath(Process p)
    {
        // QueryFullProcessImageName needs only limited access, so it also reads processes MainModule cannot open
        var limited = ProcessImage.TryPath(p.Id);
        if (limited is not null) return limited;
        try { return p.MainModule?.FileName; }
        catch (Exception ex) when (ex is Win32Exception or InvalidOperationException or NotSupportedException) { return null; }
    }
}

/// <summary>The full image path of a process, read with the limited-information right (works where Process.MainModule is refused).</summary>
internal static class ProcessImage
{
    private const uint QueryLimitedInformation = 0x1000;

    public static string? TryPath(int pid)
    {
        var handle = OpenProcess(QueryLimitedInformation, false, pid);
        if (handle == IntPtr.Zero) return null;
        try
        {
            var buffer = new StringBuilder(1024);
            var size = (uint)buffer.Capacity;
            return QueryFullProcessImageNameW(handle, 0, buffer, ref size) ? buffer.ToString(0, (int)size) : null;
        }
        finally { CloseHandle(handle); }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, int pid);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder name, ref uint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);
}
