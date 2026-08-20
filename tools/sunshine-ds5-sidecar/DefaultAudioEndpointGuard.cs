using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace Sunshine.Ds5Sidecar;

/// <summary>
/// Fails closed when Windows selects the virtual HIDMaestro DualSense speaker
/// as a default render endpoint. This is intentionally read-only: changing
/// Windows audio policy relies on undocumented APIs and would make the helper
/// responsible for restoring user preferences after crashes or upgrades.
/// </summary>
internal sealed class DefaultAudioEndpointGuard : IDisposable
{
    internal enum AudioRole : byte
    {
        Console = 0,
        Multimedia = 1,
        Communications = 2,
    }

    internal readonly record struct DeviceNodeIdentity(string InstanceId, IReadOnlyList<string> HardwareIds);

    private static readonly Guid MmDeviceEnumeratorClass =
        new("BCDE0395-E52F-467C-8E3D-C4579291692E");
    private readonly CancellationTokenSource _stopping = new();
    private readonly Action<AudioRole> _onViolation;
    private readonly Task _worker;
    private int _reported;

    internal DefaultAudioEndpointGuard(Action<AudioRole> onViolation)
    {
        _onViolation = onViolation;
        _worker = Task.Run(MonitorAsync);
    }

    private async Task MonitorAsync()
    {
        IMMDeviceEnumerator? enumerator = null;
        try
        {
            var type = Type.GetTypeFromCLSID(MmDeviceEnumeratorClass, throwOnError: true)!;
            enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(type)!;
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(400));
            do
            {
                foreach (var role in Enum.GetValues<AudioRole>())
                {
                    if (IsDefaultVirtualDualSense(enumerator, role) &&
                        Interlocked.Exchange(ref _reported, 1) == 0)
                    {
                        _onViolation(role);
                        return;
                    }
                }
            }
            while (await timer.WaitForNextTickAsync(_stopping.Token));
        }
        catch (OperationCanceledException) when (_stopping.IsCancellationRequested)
        {
            // Normal session teardown.
        }
        catch (Exception error)
        {
            // Endpoint policy monitoring is defensive. A transient Core Audio
            // failure must not take down controller input.
            Console.Error.WriteLine($"Unable to monitor the default audio endpoint: {error.Message}");
        }
        finally
        {
            if (enumerator is not null && Marshal.IsComObject(enumerator))
                Marshal.FinalReleaseComObject(enumerator);
        }
    }

    private static bool IsDefaultVirtualDualSense(IMMDeviceEnumerator enumerator, AudioRole role)
    {
        IMMDevice? endpoint = null;
        try
        {
            // AUDCLNT_E_DEVICE_INVALIDATED and E_NOTFOUND are normal while an
            // endpoint is being created or removed, so treat any failed lookup
            // as "not currently default" and retry on the next poll.
            if (enumerator.GetDefaultAudioEndpoint(DataFlow.Render, role, out endpoint) < 0 || endpoint is null ||
                endpoint.GetId(out var endpointId) < 0)
                return false;
            return IsVirtualDualSenseEndpoint(endpointId);
        }
        finally
        {
            if (endpoint is not null && Marshal.IsComObject(endpoint))
                Marshal.FinalReleaseComObject(endpoint);
        }
    }

    internal static bool IsVirtualDualSenseEndpoint(string endpointId)
    {
        var instanceId = endpointId.StartsWith("SWD\\MMDEVAPI\\", StringComparison.OrdinalIgnoreCase)
            ? endpointId
            : "SWD\\MMDEVAPI\\" + endpointId;
        if (CM_Locate_DevNodeW(out var node, instanceId, 0) != 0)
            return false;

        var chain = new List<DeviceNodeIdentity>();
        for (var depth = 0; depth < 12; ++depth)
        {
            var currentId = GetDeviceId(node);
            if (currentId is null)
                break;
            chain.Add(new DeviceNodeIdentity(currentId, ReadHardwareIds(currentId)));
            if (CM_Get_Parent(out node, node, 0) != 0)
                break;
        }
        return IsVirtualDualSenseChain(chain);
    }

    internal static bool IsVirtualDualSenseChain(IEnumerable<DeviceNodeIdentity> chain)
    {
        var sonyDualSense = false;
        var hidMaestro = false;
        foreach (var node in chain)
        {
            sonyDualSense |= Contains(node.InstanceId, "VID_054C&PID_0CE6") ||
                             node.HardwareIds.Any(id => Contains(id, "VID_054C&PID_0CE6"));
            hidMaestro |= node.HardwareIds.Any(id =>
                id.Equals("ROOT\\HIDMAESTRO_UDE", StringComparison.OrdinalIgnoreCase) ||
                id.StartsWith("ROOT\\HIDMAESTRO_UDE\\", StringComparison.OrdinalIgnoreCase));
        }
        // Checking both properties avoids rejecting a physical Sony controller
        // or some unrelated HIDMaestro virtual device.
        return sonyDualSense && hidMaestro;
    }

    private static bool Contains(string value, string needle) =>
        value.Contains(needle, StringComparison.OrdinalIgnoreCase);

    private static string? GetDeviceId(uint node)
    {
        if (CM_Get_Device_ID_Size(out var length, node, 0) != 0)
            return null;
        var buffer = new StringBuilder(checked((int)length + 1));
        return CM_Get_Device_IDW(node, buffer, length + 1, 0) == 0 ? buffer.ToString() : null;
    }

    private static IReadOnlyList<string> ReadHardwareIds(string instanceId)
    {
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Enum\" + instanceId, writable: false);
            return key?.GetValue("HardwareID") switch
            {
                string[] values => values,
                string value => new[] { value },
                _ => Array.Empty<string>(),
            };
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            return Array.Empty<string>();
        }
    }

    public void Dispose()
    {
        _stopping.Cancel();
        try { _worker.GetAwaiter().GetResult(); }
        catch (OperationCanceledException) { }
        _stopping.Dispose();
    }

    private enum DataFlow
    {
        Render = 0,
    }

    [ComImport]
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceEnumerator
    {
        [PreserveSig]
        int EnumAudioEndpoints(DataFlow dataFlow, uint stateMask, out IntPtr devices);
        [PreserveSig]
        int GetDefaultAudioEndpoint(DataFlow dataFlow, AudioRole role, out IMMDevice endpoint);
        [PreserveSig]
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice endpoint);
        [PreserveSig]
        int RegisterEndpointNotificationCallback(IntPtr callback);
        [PreserveSig]
        int UnregisterEndpointNotificationCallback(IntPtr callback);
    }

    [ComImport]
    [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDevice
    {
        [PreserveSig]
        int Activate(ref Guid interfaceId, uint classContext, IntPtr activationParameters,
                     [MarshalAs(UnmanagedType.IUnknown)] out object instance);
        [PreserveSig]
        int OpenPropertyStore(uint access, out IntPtr properties);
        [PreserveSig]
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig]
        int GetState(out uint state);
    }

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    private static extern uint CM_Locate_DevNodeW(out uint deviceInstance,
                                                  string deviceId,
                                                  uint flags);

    [DllImport("cfgmgr32.dll")]
    private static extern uint CM_Get_Parent(out uint parentDeviceInstance,
                                             uint deviceInstance,
                                             uint flags);

    [DllImport("cfgmgr32.dll")]
    private static extern uint CM_Get_Device_ID_Size(out uint length,
                                                     uint deviceInstance,
                                                     uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    private static extern uint CM_Get_Device_IDW(uint deviceInstance,
                                                 StringBuilder buffer,
                                                 uint bufferLength,
                                                 uint flags);
}
