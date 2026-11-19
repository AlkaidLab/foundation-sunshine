using System.ComponentModel;
using System.Diagnostics;

namespace Sparxie.Launch;

/// <summary>
/// The safety net: when Sparxie cannot start the session, the game is started as Sunshine's own plain application would (elevated, the working directory Sunshine gave the app),
/// without the touch runtime. The launcher then follows the game, so Sunshine still sees one running app and its exit.
/// Endfield is started the way the Collapse Launcher's Endfield plugin starts it (<see cref="DecoyParent"/>): a suspended Games.exe is the game's parent, so the
/// client's check of its parent process passes without the official launcher; if that cannot be set up, the plain start below is the last resort and the log says so.
/// This is also the whole of the host's own Endfield direct start (<c>sparxie-launch --endfield-direct</c>).
/// </summary>
internal static class DirectLaunch
{
    public static string WorkingDirectory(string cwd, string exePath) =>
        !string.IsNullOrWhiteSpace(cwd) && Directory.Exists(cwd) ? cwd : (Path.GetDirectoryName(exePath) ?? ".");

    public static ProcessPriorityClass PriorityClass(string priority) => priority switch
    {
        "belowNormal" => ProcessPriorityClass.BelowNormal,
        "aboveNormal" => ProcessPriorityClass.AboveNormal,
        "high" => ProcessPriorityClass.High,
        _ => ProcessPriorityClass.Normal,
    };

    /// <summary>True for the titles whose client checks its parent process and is started under a decoy parent.</summary>
    public static bool UsesDecoyParent(Profile profile) => string.Equals(profile.Game, "endfield", StringComparison.OrdinalIgnoreCase);

    /// <summary>Starts the game and waits for it; the result is the game's exit code (or a launcher code when it never started).</summary>
    /// <param name="parentSource">Endfield only: the image copied to Games.exe (default PlatformProcess.exe in the game's folder).</param>
    /// <param name="verifySigner">Endfield only: false skips the Authenticode checks (tests).</param>
    /// <param name="gameArguments">The game's command line from the host's screen plan (`--endfield-direct ... -- args`); null takes the profile's own screen arguments.</param>
    public static async Task<int> RunAsync(Profile profile, RunLog log, bool elevated, string cwd, string? parentSource = null, bool verifySigner = true, string? gameArguments = null)
    {
        var exe = profile.ExecutablePath;
        if (!File.Exists(exe))
        {
            log.Error($"direct start: the game executable does not exist: {exe}");
            return ExitCodes.Config;
        }
        var dir = WorkingDirectory(cwd, exe);
        JobGuard? job = null;
        try { job = JobGuard.Create(); } catch (Exception ex) { log.Warn($"direct start: no job object ({ex.Message}); the game will not stop with the launcher on its own"); }
        var arguments = gameArguments ?? profile.Screen?.Arguments() ?? "";
        Process? game = null;
        DecoyParent? decoy = null;
        try
        {
            if (UsesDecoyParent(profile)) game = TryStartUnderDecoy(exe, parentSource, verifySigner, arguments, job, log, out decoy);
            try
            {
                game ??= Process.Start(new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = dir, Arguments = arguments })
                    ?? throw new InvalidOperationException("Process.Start returned nothing");
            }
            catch (Win32Exception ex) when (ex.NativeErrorCode == 740 && !elevated)
            {
                log.Warn("direct start: the game needs administrator rights and the launcher is not elevated; asking Windows (a prompt may appear)");
                game = Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true, Verb = "runas", WorkingDirectory = dir, Arguments = arguments })
                    ?? throw new InvalidOperationException("Process.Start returned nothing");
            }
            log.Info($"direct start: game started, pid={game.Id}, working directory {dir}" + (arguments.Length > 0 ? $", arguments {arguments}" : ""));
            StageReport.Report(StageReport.WaitingWindow);
            StageReport.Pids([game.Id]);
            var joined = job?.TryAssign(game.Id);
            if (job is not null) log.Info(joined is null ? "direct start: the game is in the job object" : $"direct start: the game could not join the job object: {joined}");
            try { game.PriorityClass = PriorityClass(profile.Hoyo?.Priority ?? "normal"); }
            catch (Exception ex) when (ex is Win32Exception or InvalidOperationException) { }

            await game.WaitForExitAsync().ConfigureAwait(false);
            var code = game.ExitCode;
            log.Info($"direct start: the game exited with code {code}");
            // a game that hands over to a second process of the same executable is followed until that one ends too
            while (BrokerProcess.FindGameProcesses(exe).Count > 0) await Task.Delay(1000).ConfigureAwait(false);
            return code;
        }
        catch (Exception ex) when (ex is InvalidOperationException or Win32Exception)
        {
            log.Error($"direct start: the game could not be started: {ex.Message}");
            return ExitCodes.Internal;
        }
        finally
        {
            game?.Dispose();
            job?.Terminate();
            job?.Dispose();
            // the decoy parent ends with the game: it is terminated and its session folder removed
            decoy?.Dispose();
        }
    }

    /// <summary>The Collapse plugin's method: the game's parent is a suspended Games.exe. Returns null (and logs why) when it cannot be set up; the caller then starts the game plainly.</summary>
    private static Process? TryStartUnderDecoy(string exe, string? parentSource, bool verifySigner, string arguments, JobGuard? job, RunLog log, out DecoyParent? decoy)
    {
        decoy = null;
        try
        {
            decoy = DecoyParent.Create(exe, parentSource, verifySigner);
            log.Info($"direct start: decoy parent ready, pid={decoy.Pid}, {decoy.StagedPath}");
            var inJob = job?.TryAssign(decoy.Pid);
            if (job is not null) log.Info(inJob is null ? "direct start: the decoy parent is in the job object" : $"direct start: the decoy parent could not join the job object: {inJob}");
            var game = decoy.StartGame(exe, arguments.Length > 0 ? arguments : null, Path.GetDirectoryName(Path.GetFullPath(exe)));
            log.Info($"direct start: the game was created with the decoy parent (pid {decoy.Pid}) as its parent process");
            return game;
        }
        catch (Exception ex) when (ex is Win32Exception or IOException or InvalidDataException or InvalidOperationException or UnauthorizedAccessException)
        {
            log.Warn($"direct start: the decoy parent method failed ({ex.Message}); starting the game plainly, which the client may refuse (error 10100)");
            decoy?.Dispose();
            decoy = null;
            return null;
        }
    }
}
