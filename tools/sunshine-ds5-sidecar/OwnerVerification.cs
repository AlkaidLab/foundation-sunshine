using System.IO.Pipes;
using System.Runtime.InteropServices;

namespace Sunshine.Ds5Sidecar;

/// <summary>
/// The pipe ACL already restricts callers to the creating user. This check
/// additionally requires an elevated client so a non-elevated process of the
/// same user cannot drive the elevated sidecar (driver install, virtual HID
/// creation) even if it wins the single-instance pipe race.
/// </summary>
internal static class OwnerVerification
{
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint TokenQuery = 0x0008;
    private const int TokenElevation = 20;

    internal static bool ClientIsElevated(NamedPipeServerStream pipe)
    {
        if (!GetNamedPipeClientProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out var clientId))
            return false;
        // The protocol self-test connects from inside this process; Program
        // refuses to run it unelevated.
        if (clientId == (uint)Environment.ProcessId)
            return true;

        var process = OpenProcess(ProcessQueryLimitedInformation, false, clientId);
        if (process == IntPtr.Zero)
            return false;
        try
        {
            if (!OpenProcessToken(process, TokenQuery, out var token) || token == IntPtr.Zero)
                return false;
            try
            {
                var elevation = new byte[4];
                return GetTokenInformation(token, TokenElevation, elevation, (uint)elevation.Length, out _) &&
                       BitConverter.ToInt32(elevation, 0) != 0;
            }
            finally
            {
                CloseHandle(token);
            }
        }
        finally
        {
            CloseHandle(process);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeClientProcessId(IntPtr pipe, out uint clientProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, uint processId);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, uint desiredAccess, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetTokenInformation(IntPtr token, int informationClass,
        byte[] information, uint informationLength, out uint returnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
}
