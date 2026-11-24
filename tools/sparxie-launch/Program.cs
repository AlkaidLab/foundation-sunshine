namespace Sparxie.Launch;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        var exeDirectory = AppContext.BaseDirectory;
        string? Env(string name) => Environment.GetEnvironmentVariable(name);
        // crash reports first, so a failure of anything below leaves one (CrashReport.cs)
        CrashReport.Install(exeDirectory, Env);
        CrashReport.RunTestSwitch(exeDirectory, Env);
        CrashReport.StartUploader(exeDirectory, Env);
        using var log = RunLog.Open(RunLog.CandidateDirectories(exeDirectory, Env));
        try
        {
            if (log.Path is not null) log.Info($"log file: {log.Path}");
            else log.Warn("no log folder could be written");
            return await Launcher.RunAsync(args, log, Env, exeDirectory).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            log.Error($"result: exit {ExitCodes.Internal} - unexpected {ex.GetType().Name}: {ex.Message}");
            return ExitCodes.Internal;
        }
    }
}
