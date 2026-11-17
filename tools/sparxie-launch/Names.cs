using System.Text.RegularExpressions;

namespace Sparxie.Launch;

/// <summary>The stage and error names behind the numbers in Sparxie's events (Sparxie.Contracts.Errors, version 1), for the log.</summary>
internal static class SparxieNames
{
    private static readonly string[] Stages =
    [
        "Validation", "Mutex", "RecoveryRecord", "Job", "CreateProcess", "TouchScan", "FpsScan", "Patch", "Inject", "Restore", "Running", "Wait", "HostFault", "Cleanup",
    ];

    private static readonly string[] Errors =
    [
        "None", "InvalidArgument", "Unauthorized", "ProtocolVersionMismatch", "ConfigDirectoryNotWritable", "ConfigReadFailed", "ConfigWriteFailed", "ConfigInvalid",
        "ConfigBackupFailed", "GameAlreadyRunning", "MutexConflict", "ProfileNotFound", "SessionNotFound", "InvalidExecutableName", "ExecutableNotFound", "JobSetupFailed",
        "ProcessCreateFailed", "TouchScanFailed", "FpsScanFailed", "PatchFailed", "InjectFailed", "InstallConfirmFailed", "RecoveryRecordPresent", "RecoveryAssetCorrupt",
        "RecoveryPathInvalid", "RecoveryGameStillRunning", "RecoveryRestoreFailed", "ZzzRecoveryFailed", "HostCrashedBeforeRunning", "HostCrashedAfterRunning", "RuntimeLoadFailed",
    ];

    // HoyoTouchError of the HoyoTouchCore C ABI, version 1
    private static readonly string[] Hoyo =
    [
        "OK", "INVALID_ARGUMENT", "ABI_MISMATCH", "CONFIG_INVALID", "SCAN_FAILED", "PATCH_FAILED", "INSTALL_CONFIRM_FAILED", "PROCESS_NOT_FOUND", "SESSION_NOT_ACTIVE", "NOT_SUPPORTED", "INTERNAL",
    ];

    public static string Stage(int n) => n >= 0 && n < Stages.Length ? $"{Stages[n]}({n})" : $"stage {n}";

    public static string Error(int n) => n >= 0 && n < Errors.Length ? $"{Errors[n]}({n})" : $"error {n}";

    /// <summary>"Hoyo ...失败: 6 ..." names the runtime's own code; null when the message carries none.</summary>
    public static string? HoyoCode(string message)
    {
        if (!message.StartsWith("Hoyo", StringComparison.Ordinal)) return null;
        var m = Regex.Match(message, @"[:：]\s*(\d{1,2})(?!\d)");
        if (!m.Success || !int.TryParse(m.Groups[1].Value, out var n) || n < 1 || n >= Hoyo.Length) return null;
        return $"Hoyo runtime error {n} = {Hoyo[n]}";
    }
}
