using System.Text;
using System.Text.Json;

namespace Sparxie.Launch;

/// <summary>
/// Tells the host where the launch is, so the launch splash (docs/launch-splash.md) can say it: the host names a file in SUNSHINE_LAUNCH_STATUS_FILE and its launch
/// in SUNSHINE_LAUNCH_ID, and this writes the whole file again at each change (beside it first, then moved over, so the host never reads half of it). Without the
/// variables (Sparxie started by hand, an older host) every call does nothing. A write that fails is not an error of the launch: the splash only loses its lines.
/// </summary>
internal static class StageReport
{
    public const string FileEnv = "SUNSHINE_LAUNCH_STATUS_FILE";
    public const string IdEnv = "SUNSHINE_LAUNCH_ID";

    public const string Broker = "broker";                 // the launcher starts Sparxie's Broker
    public const string Preparing = "preparing";           // Sparxie prepares the session
    public const string Injecting = "injecting";           // Sparxie injects its touch layer into the game
    public const string WaitingWindow = "waiting-window";  // the game runs; its window is not up yet
    public const string Failed = "failed";

    private static readonly string[] Order = [Broker, Preparing, Injecting, WaitingWindow];

    /// <summary>
    /// The Broker's stage numbers of the injection step. They are Sparxie's own protocol and not in this repository: only Starting (4) and Running (5) are known
    /// from the Broker's replies, so the splash goes straight from preparing to waiting-window until the numbers are filled in here.
    /// </summary>
    public static readonly HashSet<int> InjectingStages = [];

    private static readonly object Gate = new();
    private static Func<string, string?> _env = Environment.GetEnvironmentVariable;
    private static string? _stage;
    private static string? _notice;
    private static string? _reason;
    private static List<int> _pids = [];

    /// <summary>For the tests: another environment, and a clean slate.</summary>
    internal static void Reset(Func<string, string?>? env = null)
    {
        lock (Gate)
        {
            _env = env ?? Environment.GetEnvironmentVariable;
            _stage = null; _notice = null; _reason = null; _pids = [];
        }
    }

    /// <summary>The stage a Broker event belongs to, or null for one that says nothing new (Exited and the failures end the launch another way).</summary>
    public static string? StageOfEvent(string state, int stage)
    {
        if (InjectingStages.Contains(stage) && state == "Starting") return Injecting;
        return state switch
        {
            "Starting" => Preparing,
            "Running" => WaitingWindow,
            _ => null,
        };
    }

    /// <summary>A short code for a launcher exit code; the Panel turns it into a sentence.</summary>
    public static string ReasonOfExit(int exitCode) => exitCode switch
    {
        ExitCodes.Usage => "usage",
        ExitCodes.Config => "config",
        ExitCodes.Broker => "broker",
        ExitCodes.Rejected => "rejected",
        ExitCodes.Rpc => "rpc",
        ExitCodes.SessionFailed => "session-failed",
        ExitCodes.Lost => "lost",
        ExitCodes.TimedOut => "timed-out",
        _ => "internal",
    };

    /// <summary>Reports a stage. A stage that is not later than the one reported before is dropped.</summary>
    public static void Report(string stage)
    {
        lock (Gate)
        {
            if (_stage == Failed) return;
            var next = Array.IndexOf(Order, stage);
            var now = _stage is null ? -1 : Array.IndexOf(Order, _stage);
            if (next < 0 || next <= now) return;
            _stage = stage;
            Write();
        }
    }

    public static void ReportEvent(string state, int stage)
    {
        var mapped = StageOfEvent(state, stage);
        if (mapped is not null) Report(mapped);
    }

    public static void Pids(IEnumerable<int> pids)
    {
        lock (Gate)
        {
            _pids = pids.Where(p => p > 0).Distinct().Take(16).ToList();
            Write();
        }
    }

    /// <summary>A line that does not end the launch (code <c>direct</c>: the game was started without Sparxie).</summary>
    public static void Notice(string code)
    {
        lock (Gate)
        {
            _notice = code;
            Write();
        }
    }

    /// <summary>The launch did not work; nothing is reported after this.</summary>
    public static void Fail(int exitCode)
    {
        lock (Gate)
        {
            if (_stage == Failed) return;
            _stage = Failed;
            _reason = ReasonOfExit(exitCode);
            Write();
        }
    }

    /// <summary>The JSON the host reads (docs/launch-splash.md), or null when the host asked for nothing.</summary>
    internal static string? Build(string? launchId)
    {
        if (string.IsNullOrEmpty(launchId)) return null;
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream))
        {
            w.WriteStartObject();
            w.WriteString("launch", launchId);
            if (_stage is not null) w.WriteString("stage", _stage);
            if (_reason is not null) w.WriteString("reason", _reason);
            if (_notice is not null) w.WriteString("notice", _notice);
            w.WriteStartArray("pids");
            foreach (var pid in _pids) w.WriteNumberValue(pid);
            w.WriteEndArray();
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    // called with the gate held
    private static void Write()
    {
        var path = _env(FileEnv);
        var json = Build(_env(IdEnv));
        if (string.IsNullOrWhiteSpace(path) || json is null) return;
        try
        {
            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            var temp = path + ".tmp";
            File.WriteAllText(temp, json, new UTF8Encoding(false));
            File.Move(temp, path, true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Security.SecurityException or NotSupportedException or ArgumentException)
        {
            // the host's folder may not be writable for this user; the launch goes on without its lines
        }
    }
}
