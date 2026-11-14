using System.Diagnostics;
using System.Reflection;
using System.Security.Principal;

namespace Sparxie.Launch;

internal enum Outcome
{
    Completed,   // the session ran and the game exited normally
    Ended,       // the game had started and the session ended badly (crash after Running): never start a second copy
    NotStarted,  // nothing reached Running
}

internal sealed record AttemptResult(Outcome Outcome, int ExitCode, string Reason);

internal enum Next { Done, Retry, Fallback, FollowRunningGame }

internal enum Start { NewSession, AdoptRunningGame }

/// <summary>What follows an attempt. A launch must not end in failure while the game can still be started.</summary>
internal static class Plan
{
    /// <summary>
    /// What a launch does first. The same profile's game may already be running, started by an earlier session of ours whose host died (the stream's Sunshine crashed and
    /// the service restarted it): its Sparxie session is still alive, so the Broker would refuse a second one (MutexConflict, GameAlreadyRunning) and a direct start
    /// would be a second copy. The launch joins the running game instead of failing twice.
    /// </summary>
    public static Start AtStart(bool gameRunning) => gameRunning ? Start.AdoptRunningGame : Start.NewSession;

    public static Next After(int attempt, Outcome outcome, bool gameRunning, bool brokerAvailable)
    {
        if (outcome != Outcome.NotStarted) return Next.Done;
        if (gameRunning) return Next.FollowRunningGame;
        if (attempt == 1 && brokerAvailable) return Next.Retry;
        return Next.Fallback;
    }
}

/// <summary>
/// Starts one Sparxie profile through the Broker and stays alive while the game runs, so Sunshine tracks the launcher as the app.
/// It ends with the game; if Sunshine ends the launcher first, the job object ends the game. If Sparxie cannot start the session, the launcher tries once more
/// (a fresh Broker, nothing of ours around it) and then starts the game directly, so the stream goes on.
/// </summary>
internal static class Launcher
{
    public static readonly TimeSpan BrokerReadyLimit = TimeSpan.FromSeconds(20);
    public static readonly TimeSpan PingLimit = TimeSpan.FromSeconds(2);
    public static readonly TimeSpan StartSessionLimit = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan RunningLimit = TimeSpan.FromSeconds(120);
    public static readonly TimeSpan ShutdownGrace = TimeSpan.FromSeconds(3);

    public static async Task<int> RunAsync(string[] args, RunLog log, Func<string, string?> env, string exeDirectory)
    {
        var version = typeof(Launcher).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "?";
        bool elevated;
        try { elevated = BrokerProcess.IsElevated(); } catch (Exception) { elevated = false; }
        string user;
        try { user = WindowsIdentity.GetCurrent().Name; } catch (Exception) { user = "?"; }
        log.Info($"sparxie-launch {version} pid={Environment.ProcessId} session={Process.GetCurrentProcess().SessionId} user={user} elevated={elevated} cwd={Environment.CurrentDirectory}");
        log.Info("args: " + string.Join(" ", args.Select(a => a.Contains(' ') ? "\"" + a + "\"" : a)));

        // the host's own Endfield direct start (no Sparxie session): the game under a decoy parent, as the Collapse Launcher's Endfield plugin starts it
        if (EndfieldDirect.IsRequested(args)) return await EndfieldDirect.RunAsync(args, log, elevated).ConfigureAwait(false);

        var options = LaunchOptions.Parse(args, out var optionError);
        if (options is null) return Finish(log, ExitCodes.Usage, optionError);

        // the profile
        var dataDirs = SparxieConfig.DataDirectories(options.SparxiePath, env(SparxieConfig.DataDirEnv), env("LOCALAPPDATA"));
        var dataDir = SparxieConfig.PickDataDirectory(dataDirs);
        var configPath = Path.Combine(dataDir, "config.json");
        log.Info($"sparxie={options.SparxiePath} profile={options.ProfileId} dataDir={dataDir} config={configPath}");
        Profile profile;
        try { profile = SparxieConfig.Load(configPath, options.ProfileId); }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException)
        { return Finish(log, ExitCodes.Config, ex.Message); }
        log.Info($"profile found: id={profile.Id} name={profile.DisplayName} game={profile.Game} exe={profile.ExecutablePath}");
        // an Endfield session learns the client's device and the stream's size here; the other games are unchanged
        profile = EndfieldSession.Resolve(profile, env);
        // the window and size the host chose for this stream (Unity arguments of the game; the registry write of the host agrees with them)
        if (options.ScreenWarning is not null) log.Warn($"screen arguments ignored: {options.ScreenWarning}");
        if (options.Screen is { } requested)
        {
            profile = profile with { Screen = requested };
            log.Info($"screen arguments: {requested.Arguments()}");
        }
        log.Info("settings: " + (profile.Hoyo?.Describe() ?? "none (the profile carries no hoyo block)"));
        if (!File.Exists(profile.ExecutablePath)) log.Warn($"the game executable does not exist: {profile.ExecutablePath}");

        var noticeDirs = LaunchNotice.CandidateDirectories(exeDirectory, env).ToList();
        void Note(string kind, string reason)
        {
            var json = LaunchNotice.Build(kind, profile, reason, log.Path, DateTime.UtcNow);
            var where = LaunchNotice.Write(noticeDirs, json);
            if (kind == LaunchNotice.KindDirect) StageReport.Notice("direct");
            log.Info(where is null ? $"host notice ({kind}) could not be written" : $"host notice ({kind}) written to {where}: {LaunchNotice.Messages(kind).Zh}");
        }

        if (Plan.AtStart(GameRunning(profile)) == Start.AdoptRunningGame)
            return await AdoptAsync(profile, log, noticeDirs).ConfigureAwait(false);

        var brokerPath = BrokerProcess.Locate(options.SparxiePath);
        if (brokerPath is null)
        {
            log.Warn($"Sparxie.Broker.exe was not found beside {options.SparxiePath} or in an app-* folder");
            return await FallBackAsync(profile, log, elevated, Note, "Sparxie.Broker.exe was not found").ConfigureAwait(false);
        }
        log.Info($"broker path: {brokerPath}");

        // touch only is not supported by the installed Hoyo runtime: say so, and send what it can run
        var hash = PureTouch.RuntimeHash(brokerPath);
        log.Info($"HoyoTouchCore.dll sha256={hash ?? "not found"}");
        var sent = profile;
        var unlockForced = false;
        if (PureTouch.IsTouchOnly(profile) && hash is not null && PureTouch.KnownBrokenRuntimes.Contains(hash))
        {
            sent = PureTouch.WithUnlock(profile);
            unlockForced = true;
            log.Warn($"this HoyoTouchCore.dll fails touch-only sessions (Hoyo error 6); the unlock is sent on with {PureTouch.GameOwnCap} FPS, the game's own cap");
        }

        var sparxieLogs = new SparxieLogs(SparxieLogs.DirectoryFor(brokerPath));

        // attempt 1
        log.LinePrefix = "[1] ";
        var first = await AttemptAsync(sent, brokerPath, useJob: true, elevated, log, sparxieLogs).ConfigureAwait(false);
        log.LinePrefix = "";
        if (first.Outcome != Outcome.NotStarted)
        {
            if (first.Outcome == Outcome.Completed && unlockForced) Note(LaunchNotice.KindUnlock, first.Reason);
            else if (first.Outcome == Outcome.Completed) LaunchNotice.Clear(noticeDirs);
            return Finish(log, first.ExitCode, first.Reason);
        }
        log.Warn($"attempt 1 ended before the game started: {first.Reason}");
        foreach (var line in BrokerProcess.DescribeCandidates(profile.ExecutablePath)) log.Info("candidate: " + line);

        // a failed start ends its game process a moment later; only a game that stays is one to follow, never one to start twice
        await WaitGameGoneAsync(profile, TimeSpan.FromSeconds(4)).ConfigureAwait(false);
        // an installed Sparxie that lacks a feature the profile needs will not change on a second try: go straight to the direct start
        var brokerCanRetry = first.ExitCode != ExitCodes.Unsupported;
        var afterFirst = Plan.After(1, first.Outcome, GameRunning(profile), brokerAvailable: brokerCanRetry);
        if (afterFirst == Next.FollowRunningGame)
            return await FollowAsync(profile, log, "a process of the game is still running", Note).ConfigureAwait(false);
        if (afterFirst == Next.Fallback)
            return await FallBackAsync(profile, log, elevated, Note, $"attempt 1: {first.Reason}").ConfigureAwait(false);

        // attempt 2: a fresh Broker, nothing of ours around the session, the safest settings
        var retryProfile = PureTouch.WithUnlock(sent);
        log.Info("retrying once: new Broker, no job object of ours" + (PureTouch.IsTouchOnly(sent) ? $", unlock on with {PureTouch.GameOwnCap} FPS" : ""));
        log.LinePrefix = "[2] ";
        var second = await AttemptAsync(retryProfile, brokerPath, useJob: false, elevated, log, sparxieLogs).ConfigureAwait(false);
        log.LinePrefix = "";
        if (second.Outcome != Outcome.NotStarted)
        {
            if (second.Outcome == Outcome.Completed) Note(LaunchNotice.KindRetried, $"attempt 1: {first.Reason}");
            return Finish(log, second.ExitCode, second.Reason);
        }
        log.Warn($"attempt 2 ended before the game started: {second.Reason}");
        foreach (var line in BrokerProcess.DescribeCandidates(profile.ExecutablePath)) log.Info("candidate: " + line);
        await WaitGameGoneAsync(profile, TimeSpan.FromSeconds(4)).ConfigureAwait(false);
        if (Plan.After(2, second.Outcome, GameRunning(profile), brokerAvailable: true) == Next.FollowRunningGame)
            return await FollowAsync(profile, log, "a process of the game is still running", Note).ConfigureAwait(false);
        return await FallBackAsync(profile, log, elevated, Note, $"attempt 1: {first.Reason}; attempt 2: {second.Reason}").ConfigureAwait(false);
    }

    private static bool GameRunning(Profile profile) => BrokerProcess.FindGameProcesses(profile.ExecutablePath).Count > 0;

    private static async Task WaitGameGoneAsync(Profile profile, TimeSpan limit)
    {
        var until = DateTime.UtcNow + limit;
        while (GameRunning(profile) && DateTime.UtcNow < until) await Task.Delay(250).ConfigureAwait(false);
    }

    private static async Task<int> FallBackAsync(Profile profile, RunLog log, bool elevated, Action<string, string> note, string reason)
    {
        log.Warn($"Sparxie could not start the session ({reason}); starting the game directly, without the touch runtime");
        note(LaunchNotice.KindDirect, reason);
        var code = await DirectLaunch.RunAsync(profile, log, elevated, Environment.CurrentDirectory).ConfigureAwait(false);
        return Finish(log, code, "direct start: the game ended");
    }

    /// <summary>
    /// The game of this profile is already running (an orphan of a crashed host, or started by hand): this stream joins it. Nothing failed, so no notice is left
    /// (the Web UI and the Panel show a launch notice only for a launch that needed the safety net). The game goes into a new job object, so it ends with this
    /// launcher exactly as in a session this launcher started.
    /// </summary>
    private static async Task<int> AdoptAsync(Profile profile, RunLog log, IEnumerable<string> noticeDirs)
    {
        var pids = BrokerProcess.FindGameProcesses(profile.ExecutablePath);
        log.Info("the game of this profile is already running (an earlier session of ours may have been orphaned by a host crash): joining it, no new session and no second copy; pids " + string.Join(",", pids));
        foreach (var line in BrokerProcess.DescribeCandidates(profile.ExecutablePath)) log.Info("candidate: " + line);
        LaunchNotice.Clear(noticeDirs);
        using var job = TryCreateJob(log);
        if (job is not null)
        {
            foreach (var pid in pids)
            {
                var reason = job.TryAssign(pid);
                log.Info(reason is null ? $"game pid {pid} is in the job object" : $"game pid {pid} could not join the job object: {reason}");
            }
        }
        var code = await WatchGameAsync(profile, log).ConfigureAwait(false);
        return Finish(log, code, "the joined game ended");
    }

    private static JobGuard? TryCreateJob(RunLog log)
    {
        try { return JobGuard.Create(); }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            log.Warn($"job object unavailable, the joined game will not stop with the launcher on its own: {ex.Message}");
            return null;
        }
    }

    private static async Task<int> FollowAsync(Profile profile, RunLog log, string why, Action<string, string> note)
    {
        log.Warn($"not starting another copy: {why}; following it");
        note(LaunchNotice.KindDirect, why);
        return await WatchGameAsync(profile, log).ConfigureAwait(false);
    }

    /// <summary>One try at a Sparxie session. It always cleans up after itself (the Broker, and the job when there is one).</summary>
    private static async Task<AttemptResult> AttemptAsync(Profile profile, string brokerPath, bool useJob, bool elevated, RunLog log, SparxieLogs sparxieLogs)
    {
        JobGuard? job = null;
        if (useJob)
        {
            try { job = JobGuard.Create(); log.Info("job object created (kill on close)"); }
            catch (Exception ex) { log.Warn($"job object unavailable, the game will not stop with the launcher on its own: {ex.Message}"); }
        }

        Process? broker = null;
        var stop = new CancellationTokenSource();
        BrokerRpc? rpc = null;
        EventStream? events = null;
        AttemptResult result;
        try
        {
            result = await RunSessionAsync(profile, brokerPath, job, elevated, log, stop, b => broker = b, e => events = e, r => rpc = r).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            result = new AttemptResult(Outcome.NotStarted, ExitCodes.Internal, $"unexpected {ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            await ShutDownAsync(log, stop, events, rpc, broker, job).ConfigureAwait(false);
        }
        if (result.Outcome != Outcome.Completed) sparxieLogs.DumpNew(log);
        return result;
    }

    private static async Task<AttemptResult> RunSessionAsync(Profile profile, string brokerPath, JobGuard? job, bool elevated, RunLog log, CancellationTokenSource stop,
        Action<Process> setBroker, Action<EventStream> setEvents, Action<BrokerRpc> setRpc)
    {
        static AttemptResult Not(int code, string reason) => new(Outcome.NotStarted, code, reason);

        var pipe = BrokerProcess.NewPipeName();
        StageReport.Report(StageReport.Broker);
        Process broker;
        try { broker = BrokerProcess.Start(brokerPath, pipe, log, elevated); }
        catch (InvalidOperationException ex) { return Not(ExitCodes.Broker, ex.Message); }
        setBroker(broker);
        log.Info($"broker started: pid={broker.Id} pipe={pipe} mode={(elevated ? "direct (already elevated)" : "runas")}");
        if (job is not null)
        {
            var joined = job.TryAssign(broker);
            log.Info(joined is null ? "broker is in the job object" : $"broker could not join the job object: {joined}");
        }

        var brokerExit = broker.WaitForExitAsync();
        var rpc = new BrokerRpc(pipe);
        setRpc(rpc);

        // the pipe
        var attempts = 0;
        PingInfo? pong = null;
        var ready = Stopwatch.StartNew();
        while (true)
        {
            attempts++;
            try
            {
                pong = await rpc.PingAsync(PingLimit).ConfigureAwait(false);
                log.Info($"ping ok after {ready.ElapsedMilliseconds} ms, attempt {attempts}: {pong}");
                break;
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or OperationCanceledException or GrpcStatusException or InvalidDataException or TimeoutException)
            {
                if (attempts <= 5 || attempts % 10 == 0) log.Info($"ping attempt {attempts} failed: {Short(ex)}");
            }
            if (broker.HasExited) return Not(ExitCodes.Broker, $"the Broker exited before it answered (exit code {ExitCode(broker)})");
            if (ready.Elapsed >= BrokerReadyLimit) return Not(ExitCodes.Broker, $"the Broker did not answer within {BrokerReadyLimit.TotalSeconds:0} s ({attempts} attempts)");
            await Task.Delay(250).ConfigureAwait(false);
        }

        // A session without the touch interface needs a Broker that knows the field; an older one would start the touch interface anyway.
        if (profile.Hoyo is { TouchUi: false } && !(pong?.Has(PingInfo.TouchUiSwitch) ?? false))
            return Not(ExitCodes.Unsupported, "this profile asks for a session without the touch interface, but the installed Sparxie does not support that (its Broker lists no touch-ui-switch capability)");

        // the screen arguments need a Broker that lists the capability; an older one would drop them silently, so the log says so and the registry stays the only lever
        if (profile.Screen is { IsEmpty: false } && !(pong?.Has(PingInfo.ScreenArgsCapability) ?? false))
        {
            log.Warn("the installed Sparxie lists no screen-args capability: the game starts without screen arguments (the host's registry write still applies)");
            profile = profile with { Screen = null };
        }

        // an Endfield session needs a Broker that knows the title; an older one would refuse it or start something else
        if (profile.Game == "endfield" && !(pong?.Has(PingInfo.Endfield) ?? false))
            return Not(ExitCodes.Unsupported, "this profile is an Endfield session, but the installed Sparxie does not support that (its Broker lists no endfield capability)");

        // the event stream first, then the session
        var events = rpc.OpenEvents(stop.Token);
        setEvents(events);
        var running = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var terminal = new TaskCompletionSource<SessionEvent?>(TaskCreationOptions.RunContinuationsAsynchronously);
        _ = Task.Run(async () =>
        {
            try
            {
                await foreach (var ev in events.ReadAsync(stop.Token).ConfigureAwait(false))
                {
                    log.Info($"event: session={ev.SessionId} state={ev.State} stage={SparxieNames.Stage(ev.Stage)} error={SparxieNames.Error(ev.ErrorCode)} message={ev.Message}");
                    StageReport.ReportEvent(ev.State, ev.Stage);
                    if (SparxieNames.HoyoCode(ev.Message) is { } hoyo) log.Info($"event detail: {hoyo}");
                    if (ev.State == "Running") running.TrySetResult(true);
                    if (ExitCodes.IsTerminal(ev.State)) { terminal.TrySetResult(ev); return; }
                }
                log.Warn("the Broker closed the event stream");
            }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                if (!stop.IsCancellationRequested) log.Warn($"the event stream broke: {Short(ex)}");
            }
            terminal.TrySetResult(null);
        });

        StartResponse answer;
        log.Info($"StartSession: profile={profile.Id} game={profile.Game} exe={profile.ExecutablePath} {(profile.Hoyo is { } h ? h.Describe() : "")}");
        try { answer = await rpc.StartSessionAsync(profile, StartSessionLimit).ConfigureAwait(false); }
        catch (Exception ex) when (ex is HttpRequestException or IOException or OperationCanceledException or GrpcStatusException or InvalidDataException)
        { return Not(ExitCodes.Rpc, $"StartSession failed: {Short(ex)}"); }
        log.Info($"StartSession answer: accepted={answer.Accepted} session={answer.SessionId} stage={SparxieNames.Stage(answer.Stage)} error={SparxieNames.Error(answer.ErrorCode)} message={answer.Message}");
        if (!answer.Accepted || string.IsNullOrEmpty(answer.SessionId))
            return Not(ExitCodes.Rejected, $"the Broker refused the session (error {SparxieNames.Error(answer.ErrorCode)}): {answer.Message}");

        // until the game is running
        var waited = Stopwatch.StartNew();
        var first = await Task.WhenAny(running.Task, terminal.Task, brokerExit, Task.Delay(RunningLimit)).ConfigureAwait(false);
        if (first == running.Task)
        {
            log.Info($"the game is running after {waited.ElapsedMilliseconds} ms");
            await GuardGameAsync(profile, job, log).ConfigureAwait(false);
            StageReport.Pids(BrokerProcess.FindGameProcesses(profile.ExecutablePath));
        }
        else if (!terminal.Task.IsCompleted)
        {
            if (first == brokerExit)
            {
                await Task.WhenAny(terminal.Task, Task.Delay(1000)).ConfigureAwait(false);
                if (!terminal.Task.IsCompleted) return Not(ExitCodes.Broker, $"the Broker exited while the game was starting (exit code {ExitCode(broker)})");
            }
            else
            {
                return Not(ExitCodes.TimedOut, $"the game did not reach Running within {RunningLimit.TotalSeconds:0} s");
            }
        }
        else if (!running.Task.IsCompleted)
        {
            // a final event with no Running before it: the session failed to start
            var early = await terminal.Task.ConfigureAwait(false);
            if (early is { State: not "Exited" })
                return Not(ExitCodes.ForState(early.State), Describe(early));
        }

        // until it ends
        var end = await Task.WhenAny(terminal.Task, brokerExit).ConfigureAwait(false);
        if (end == brokerExit && !terminal.Task.IsCompleted)
        {
            await Task.WhenAny(terminal.Task, Task.Delay(2000)).ConfigureAwait(false);
            if (!terminal.Task.IsCompleted)
            {
                log.Warn($"the Broker exited (code {ExitCode(broker)}) without a final event; watching the game process instead");
                return await WatchedAsync(profile, log).ConfigureAwait(false);
            }
        }
        var last = await terminal.Task.ConfigureAwait(false);
        if (last is null)
        {
            log.Warn("the event stream ended without a final event; watching the game process instead");
            return await WatchedAsync(profile, log).ConfigureAwait(false);
        }
        var code = ExitCodes.ForState(last.State);
        return new AttemptResult(code == ExitCodes.Exited ? Outcome.Completed : Outcome.Ended, code, Describe(last));
    }

    private static string Describe(SessionEvent ev) =>
        $"session {ev.State}" + (ev.Message.Length > 0 ? $": {ev.Message}" : "") + (ev.ErrorCode != 0 ? $" (error {SparxieNames.Error(ev.ErrorCode)}, stage {SparxieNames.Stage(ev.Stage)})" : "");

    /// <summary>The fallback when the Broker is gone: end when no process of the game executable is left.</summary>
    private static async Task<AttemptResult> WatchedAsync(Profile profile, RunLog log)
    {
        var code = await WatchGameAsync(profile, log).ConfigureAwait(false);
        return code == ExitCodes.Exited
            ? new AttemptResult(Outcome.Completed, code, "the game process ended (the Broker was gone, so no final event)")
            : new AttemptResult(Outcome.Ended, code, "the Broker is gone and no game process is running");
    }

    private static async Task GuardGameAsync(Profile profile, JobGuard? job, RunLog log)
    {
        List<int> pids = [];
        for (var i = 0; i < 10 && pids.Count == 0; i++)
        {
            pids = BrokerProcess.FindGameProcesses(profile.ExecutablePath);
            if (pids.Count == 0) await Task.Delay(300).ConfigureAwait(false);
        }
        log.Info(pids.Count == 0 ? "no process of the game executable was found" : "game process ids: " + string.Join(",", pids));
        if (job is null) return;
        foreach (var pid in pids)
        {
            var reason = job.TryAssign(pid);
            log.Info(reason is null ? $"game pid {pid} is in the job object" : $"game pid {pid} could not join the job object: {reason}");
        }
    }

    private static async Task<int> WatchGameAsync(Profile profile, RunLog log)
    {
        var pids = BrokerProcess.FindGameProcesses(profile.ExecutablePath);
        if (pids.Count == 0) return ExitCodes.Lost;
        log.Info("watching game process ids: " + string.Join(",", pids));
        StageReport.Report(StageReport.WaitingWindow);
        StageReport.Pids(pids);
        while (BrokerProcess.FindGameProcesses(profile.ExecutablePath).Count > 0) await Task.Delay(1000).ConfigureAwait(false);
        return ExitCodes.Exited;
    }

    private static async Task ShutDownAsync(RunLog log, CancellationTokenSource stop, EventStream? events, BrokerRpc? rpc, Process? broker, JobGuard? job)
    {
        try
        {
            await stop.CancelAsync().ConfigureAwait(false);
            events?.Dispose();
            rpc?.Dispose();
            if (broker is not null)
            {
                // closing the event stream lets an idle Broker stop by itself; whatever is left goes with the job, or with the kill below
                var wait = broker.WaitForExitAsync();
                var exited = broker.HasExited || await Task.WhenAny(wait, Task.Delay(ShutdownGrace)).ConfigureAwait(false) == wait;
                log.Info(exited ? "the Broker has stopped" : "the Broker did not stop by itself; ending it");
                if (!exited) { try { broker.Kill(entireProcessTree: true); } catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception) { } }
                broker.Dispose();
            }
            // what is still in the launcher's job now is what would keep Sunshine's app alive (it waits for every process of the app): say so before the job ends it
            if (job is not null)
            {
                var left = job.Members();
                if (left.Count > 0) log.Warn("still running when the launcher ends them: " + string.Join(", ", left.Select(m => $"{m.Name}({m.Id})")));
                else log.Info("nothing is left in the launcher's job");
            }
            job?.Terminate();
            job?.Dispose();
        }
        catch (Exception ex) { log.Warn($"shutdown: {ex.Message}"); }
    }

    internal static int Finish(RunLog log, int code, string reason)
    {
        if (code != ExitCodes.Exited) StageReport.Fail(code);
        if (code == ExitCodes.Exited) log.Info($"result: exit {code} - {reason}");
        else log.Error($"result: exit {code} - {reason}");
        return code;
    }

    private static string Short(Exception ex)
    {
        var inner = ex;
        while (inner.InnerException is not null) inner = inner.InnerException;
        return inner == ex ? $"{ex.GetType().Name}: {ex.Message}" : $"{ex.GetType().Name}: {ex.Message} -> {inner.GetType().Name}: {inner.Message}";
    }

    private static string ExitCode(Process p)
    {
        try { return p.ExitCode.ToString() + $" (0x{p.ExitCode:X8})"; } catch (InvalidOperationException) { return "unknown"; }
    }
}
