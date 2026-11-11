using System.Diagnostics;
using System.Runtime.InteropServices;

// Records its parent pid and the parent's image path into the file named by ECL_FAKE_RECORD, then lives ECL_FAKE_LIFE_MS ms (default 60 s).
internal static class Program
{
    private static int Main()
    {
        var record = Environment.GetEnvironmentVariable("ECL_FAKE_RECORD");
        _ = int.TryParse(Environment.GetEnvironmentVariable("ECL_FAKE_LIFE_MS"), out var life);
        var basic = new ProcessBasicInformation();
        var parent = NtQueryInformationProcess(Process.GetCurrentProcess().Handle, 0, ref basic, Marshal.SizeOf<ProcessBasicInformation>(), out _) == 0
            ? (int)basic.InheritedFromUniqueProcessId : 0;
        string image = "";
        // QueryFullProcessImageName: a suspended process has no module list yet, so Process.MainModule cannot be used
        var handle = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, false, parent);
        if (handle != 0)
        {
            var buffer = new System.Text.StringBuilder(1024);
            var size = buffer.Capacity;
            if (QueryFullProcessImageNameW(handle, 0, buffer, ref size)) image = buffer.ToString(0, size);
            CloseHandle(handle);
        }
        if (!string.IsNullOrEmpty(record))
        {
            var tmp = record + ".tmp";
            File.WriteAllLines(tmp,
            [
                $"self_pid={Environment.ProcessId}",
                $"parent_pid={parent}",
                $"parent_image={image}",
                $"cwd={Environment.CurrentDirectory}",
                $"cmdline={Environment.CommandLine}",
            ]);
            File.Move(tmp, record, true);
        }
        Thread.Sleep(life > 0 ? life : 60000);
        return 0;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessBasicInformation
    {
        public nint ExitStatus, PebBaseAddress, AffinityMask, BasePriority, UniqueProcessId, InheritedFromUniqueProcessId;
    }

    [DllImport("kernel32.dll")]
    private static extern nint OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern bool QueryFullProcessImageNameW(nint process, int flags, System.Text.StringBuilder name, ref int size);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(nint handle);

    [DllImport("ntdll.dll")]
    private static extern int NtQueryInformationProcess(nint process, int informationClass, ref ProcessBasicInformation information, int length, out int returned);
}
