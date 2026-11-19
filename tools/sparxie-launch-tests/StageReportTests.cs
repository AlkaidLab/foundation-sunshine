using System.Text.Json;

namespace Sparxie.Launch;

internal static partial class Tests
{
    private static void StageReports(string scratch)
    {
        var file = Path.Combine(scratch, "launch", "status-9-1.json");
        string? Env(string name) => name switch { StageReport.FileEnv => file, StageReport.IdEnv => "9-1", _ => null };
        JsonElement Read() => JsonDocument.Parse(File.ReadAllText(file)).RootElement.Clone();
        string? Stage() => Read().TryGetProperty("stage", out var s) ? s.GetString() : null;

        // the Broker's events map to stages; the end of a session says nothing new
        Check(StageReport.StageOfEvent("Starting", 4) == StageReport.Preparing, "stage: Starting is preparing");
        Check(StageReport.StageOfEvent("Running", 5) == StageReport.WaitingWindow, "stage: Running is waiting-window");
        Check(StageReport.StageOfEvent("Exited", 7) is null && StageReport.StageOfEvent("Failed", 13) is null, "stage: Exited and Failed report no stage");

        // exit codes become the short codes the host and the Panel know
        Check(StageReport.ReasonOfExit(ExitCodes.Broker) == "broker" && StageReport.ReasonOfExit(ExitCodes.TimedOut) == "timed-out" &&
              StageReport.ReasonOfExit(ExitCodes.SessionFailed) == "session-failed" && StageReport.ReasonOfExit(99) == "internal", "stage: exit codes map to reasons");

        // without the host's variables nothing is written
        StageReport.Reset(_ => null);
        StageReport.Report(StageReport.Broker);
        Check(!File.Exists(file), "stage: no variables, no file");

        // the file is whole, carries the launch id, and stages only advance
        StageReport.Reset(Env);
        StageReport.Report(StageReport.Broker);
        Check(File.Exists(file) && !File.Exists(file + ".tmp"), "stage: written, no temporary file left");
        Check(Read().GetProperty("launch").GetString() == "9-1" && Stage() == "broker", "stage: launch id and first stage");
        StageReport.ReportEvent("Running", 5);
        Check(Stage() == "waiting-window", "stage: Running advances to waiting-window");
        StageReport.Report(StageReport.Preparing);
        Check(Stage() == "waiting-window", "stage: an older stage is dropped");

        StageReport.Pids([0, 4242, 4242, 4343]);
        var pids = Read().GetProperty("pids").EnumerateArray().Select(p => p.GetInt32()).ToList();
        Check(pids.SequenceEqual([4242, 4343]), "stage: pids are unique and above zero");

        StageReport.Notice("direct");
        Check(Read().GetProperty("notice").GetString() == "direct" && Stage() == "waiting-window", "stage: a notice does not change the stage");

        // a failure ends the reporting
        StageReport.Fail(ExitCodes.TimedOut);
        Check(Stage() == "failed" && Read().GetProperty("reason").GetString() == "timed-out", "stage: failed with a reason");
        StageReport.Report(StageReport.WaitingWindow);
        Check(Stage() == "failed", "stage: nothing after a failure");

        // an unwritable place is not an error of the launch
        StageReport.Reset(name => name == StageReport.FileEnv ? Path.Combine(file, "x", "y.json") : "9-2");
        StageReport.Report(StageReport.Broker);
        Check(true, "stage: an unwritable status file does not throw");
        StageReport.Reset();
    }
}
