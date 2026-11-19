using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Sparxie.Launch;

/// <summary>
/// A job object that kills everything in it when its last handle closes. Sunshine ends an app by closing its window and then terminating
/// its process group; a program that is killed cannot clean up, but the operating system closes this handle for it, and so the Broker,
/// the SessionHost and the game go away with the launcher.
/// </summary>
internal sealed class JobGuard : IDisposable
{
    private const int JobObjectExtendedLimitInformation = 9;
    private const uint KillOnJobClose = 0x2000;

    private readonly SafeJobHandle _job;

    private JobGuard(SafeJobHandle job) { _job = job; }

    public static JobGuard Create()
    {
        var job = CreateJobObjectW(IntPtr.Zero, null);
        if (job.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateJobObject failed");
        var info = new ExtendedLimit { Basic = new BasicLimit { LimitFlags = KillOnJobClose } };
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref info, (uint)Marshal.SizeOf<ExtendedLimit>()))
        {
            var error = Marshal.GetLastWin32Error();
            job.Dispose();
            throw new Win32Exception(error, "SetInformationJobObject failed");
        }
        return new JobGuard(job);
    }

    /// <summary>Returns null on success, else the reason (the process may already be in a job that does not nest, or be more privileged).</summary>
    public string? TryAssign(Process process)
    {
        try
        {
            if (!AssignProcessToJobObject(_job, process.SafeHandle.DangerousGetHandle()))
                return new Win32Exception(Marshal.GetLastWin32Error()).Message;
            return null;
        }
        catch (Exception ex) when (ex is InvalidOperationException or Win32Exception) { return ex.Message; }
    }

    public string? TryAssign(int pid)
    {
        try
        {
            using var p = Process.GetProcessById(pid);
            return Contains(p) ? null : TryAssign(p);
        }
        catch (ArgumentException) { return "the process is already gone"; }
    }

    /// <summary>True when the process is already in this job (children of a member join it on their own).</summary>
    public bool Contains(Process process)
    {
        try { return IsProcessInJob(process.SafeHandle.DangerousGetHandle(), _job.DangerousGetHandle(), out var inJob) && inJob; }
        catch (Exception ex) when (ex is InvalidOperationException or Win32Exception) { return false; }
    }

    /// <summary>
    /// The processes in the job right now (id and image name), for the log: what is still alive when the launcher is about to end shows what could keep Sunshine's app running.
    /// Empty when the list cannot be read.
    /// </summary>
    public List<(int Id, string Name)> Members()
    {
        var list = new List<(int, string)>();
        if (_job.IsClosed) return list;
        const int JobObjectBasicProcessIdList = 3;
        const int capacity = 64;
        var size = 8 + capacity * IntPtr.Size;
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            if (!QueryInformationJobObject(_job, JobObjectBasicProcessIdList, buffer, (uint)size, IntPtr.Zero)) return list;
            var count = Marshal.ReadInt32(buffer, 4);
            for (var i = 0; i < count && i < capacity; i++)
            {
                var id = (int)Marshal.ReadIntPtr(buffer, 8 + i * IntPtr.Size);
                string name;
                try { using var p = Process.GetProcessById(id); name = p.ProcessName; }
                catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or Win32Exception) { name = "(gone)"; }
                list.Add((id, name));
            }
        }
        finally { Marshal.FreeHGlobal(buffer); }
        return list;
    }

    /// <summary>Kills everything still in the job.</summary>
    public void Terminate()
    {
        if (!_job.IsClosed) TerminateJobObject(_job, 1);
    }

    public void Dispose() => _job.Dispose();

    private sealed class SafeJobHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        public SafeJobHandle() : base(true) { }
        protected override bool ReleaseHandle() => CloseHandle(handle);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BasicLimit
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoCounters
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ExtendedLimit
    {
        public BasicLimit Basic;
        public IoCounters Io;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeJobHandle CreateJobObjectW(IntPtr attributes, string? name);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetInformationJobObject(SafeJobHandle job, int infoClass, ref ExtendedLimit info, uint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AssignProcessToJobObject(SafeJobHandle job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsProcessInJob(IntPtr process, IntPtr job, [MarshalAs(UnmanagedType.Bool)] out bool result);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryInformationJobObject(SafeJobHandle job, int infoClass, IntPtr info, uint size, IntPtr returnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateJobObject(SafeJobHandle job, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);
}
