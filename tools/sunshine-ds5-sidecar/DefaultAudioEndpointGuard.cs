using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Channels;
using Microsoft.Win32;

namespace Sunshine.Ds5Sidecar;

/// <summary>
/// Fails closed when Windows selects a virtual HIDMaestro DualSense audio
/// endpoint as a default render or capture endpoint. The guard is a read-only
/// fallback for systems where the documented never-default policy cannot be
/// applied.
/// </summary>
internal sealed class DefaultAudioEndpointGuard : IDisposable
{
    private const int DataFlowCount = 2;
    private const int AudioRoleCount = 3;

    internal enum AudioRole
    {
        Console = 0,
        Multimedia = 1,
        Communications = 2,
    }

    internal readonly record struct DeviceNodeIdentity(string InstanceId, IReadOnlyList<string> HardwareIds);

    private static readonly Guid MmDeviceEnumeratorClass =
        new("BCDE0395-E52F-467C-8E3D-C4579291692E");
    private readonly CancellationTokenSource _stopping = new();
    // Each flow/role pair owns one atomic latest-value slot. The capacity-one
    // channel is only a non-blocking wakeup, so notification bursts coalesce
    // without allowing a backlog to grow.
    private readonly DefaultEndpointChange?[] _pendingEndpointChanges =
        new DefaultEndpointChange?[DataFlowCount * AudioRoleCount];
    private readonly Channel<bool> _endpointChangeSignal =
        Channel.CreateBounded<bool>(new BoundedChannelOptions(1)
        {
            SingleReader = true,
            SingleWriter = false,
            FullMode = BoundedChannelFullMode.DropWrite,
        });
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
        EndpointNotificationClient? notification = null;
        var registered = false;
        try
        {
            var type = Type.GetTypeFromCLSID(MmDeviceEnumeratorClass, throwOnError: true)!;
            enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(type)!;
            notification = new EndpointNotificationClient(OnDefaultDeviceChanged);
            var registrationResult = enumerator.RegisterEndpointNotificationCallback(notification);
            registered = registrationResult >= 0;

            // Register before taking the initial snapshot so a default-device
            // change racing startup is either observed by the callback or by
            // the snapshot (and harmlessly deduplicated by _reported).
            CheckCurrentDefaults(enumerator);
            if (registered)
            {
                await ProcessEndpointChangesAsync();
            }
            else
            {
                Console.Error.WriteLine(
                    $"Unable to register the default audio endpoint monitor (0x{registrationResult:X8}); " +
                    "falling back to low-frequency polling");
                await PollDefaultsAsync(enumerator);
            }
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
            if (registered && enumerator is not null && notification is not null)
            {
                var result = enumerator.UnregisterEndpointNotificationCallback(notification);
                if (result < 0 && !_stopping.IsCancellationRequested)
                    Console.Error.WriteLine($"Unable to unregister the default audio endpoint monitor: 0x{result:X8}");
            }
            if (enumerator is not null && Marshal.IsComObject(enumerator))
                Marshal.FinalReleaseComObject(enumerator);
            GC.KeepAlive(notification);
        }
    }

    private void CheckCurrentDefaults(IMMDeviceEnumerator enumerator)
    {
        foreach (var flow in Enum.GetValues<DataFlow>())
        {
            foreach (var role in Enum.GetValues<AudioRole>())
            {
                if (TryGetDefaultEndpointId(enumerator, flow, role, out var endpointId))
                    ReportIfVirtualDualSense(role, endpointId);
            }
        }
    }

    private async Task PollDefaultsAsync(IMMDeviceEnumerator enumerator)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(2));
        while (await timer.WaitForNextTickAsync(_stopping.Token))
            CheckCurrentDefaults(enumerator);
    }

    private async Task ProcessEndpointChangesAsync()
    {
        while (await _endpointChangeSignal.Reader.WaitToReadAsync(_stopping.Token))
        {
            _endpointChangeSignal.Reader.TryRead(out _);
            for (var slot = 0; slot < _pendingEndpointChanges.Length; ++slot)
            {
                var change = Interlocked.Exchange(ref _pendingEndpointChanges[slot], null);
                if (change is null)
                    continue;
                try
                {
                    ReportIfVirtualDualSense(change.Role, change.EndpointId);
                }
                catch (Exception error)
                {
                    Console.Error.WriteLine(
                        $"Unable to inspect the changed {change.Flow} default audio endpoint: {error.Message}");
                }
                if (Volatile.Read(ref _reported) != 0)
                    return;
            }
        }
    }

    private void OnDefaultDeviceChanged(DataFlow flow, AudioRole role, string? endpointId)
    {
        if (endpointId is null || _stopping.IsCancellationRequested ||
            Volatile.Read(ref _reported) != 0 || !TryGetEndpointSlot(flow, role, out var slot))
        {
            return;
        }
        Interlocked.Exchange(
            ref _pendingEndpointChanges[slot], new DefaultEndpointChange(flow, role, endpointId));
        _endpointChangeSignal.Writer.TryWrite(true);
    }

    private static bool TryGetEndpointSlot(DataFlow flow, AudioRole role, out int slot)
    {
        var flowIndex = (int)flow;
        var roleIndex = (int)role;
        if ((uint)flowIndex >= DataFlowCount || (uint)roleIndex >= AudioRoleCount)
        {
            slot = -1;
            return false;
        }
        slot = flowIndex * AudioRoleCount + roleIndex;
        return true;
    }

    private void ReportIfVirtualDualSense(AudioRole role, string endpointId)
    {
        if (Volatile.Read(ref _reported) != 0 ||
            !IsVirtualDualSenseEndpoint(endpointId) ||
            Interlocked.Exchange(ref _reported, 1) != 0)
        {
            return;
        }
        _onViolation(role);
    }

    private static bool TryGetDefaultEndpointId(
        IMMDeviceEnumerator enumerator, DataFlow flow, AudioRole role, out string endpointId)
    {
        endpointId = string.Empty;
        IMMDevice? endpoint = null;
        try
        {
            // AUDCLNT_E_DEVICE_INVALIDATED and E_NOTFOUND are normal while an
            // endpoint is being created or removed, so treat any failed lookup
            // as "not currently default". A later notification (or the rare
            // registration-failure polling fallback) will retry it.
            if (enumerator.GetDefaultAudioEndpoint(flow, role, out endpoint) < 0 || endpoint is null ||
                endpoint.GetId(out endpointId) < 0)
                return false;
            return true;
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

        return IsVirtualDualSenseDeviceNode(node);
    }

    internal static IReadOnlyList<string> GetActiveVirtualDualSenseRenderEndpoints()
    {
        const uint deviceStateActive = 0x00000001;
        var result = new List<string>();
        IMMDeviceEnumerator? enumerator = null;
        IMMDeviceCollection? endpoints = null;
        try
        {
            var type = Type.GetTypeFromCLSID(MmDeviceEnumeratorClass, throwOnError: true)!;
            enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(type)!;
            Marshal.ThrowExceptionForHR(
                enumerator.EnumAudioEndpoints(DataFlow.Render, deviceStateActive, out endpoints));
            Marshal.ThrowExceptionForHR(endpoints.GetCount(out var count));
            for (uint index = 0; index < count; ++index)
            {
                IMMDevice? endpoint = null;
                try
                {
                    Marshal.ThrowExceptionForHR(endpoints.Item(index, out endpoint));
                    Marshal.ThrowExceptionForHR(endpoint.GetId(out var endpointId));
                    if (IsVirtualDualSenseEndpoint(endpointId))
                        result.Add(endpointId);
                }
                finally
                {
                    if (endpoint is not null && Marshal.IsComObject(endpoint))
                        Marshal.FinalReleaseComObject(endpoint);
                }
            }
            return result;
        }
        finally
        {
            if (endpoints is not null && Marshal.IsComObject(endpoints))
                Marshal.FinalReleaseComObject(endpoints);
            if (enumerator is not null && Marshal.IsComObject(enumerator))
                Marshal.FinalReleaseComObject(enumerator);
        }
    }

    internal static bool IsVirtualDualSenseDeviceNode(string instanceId, bool includePhantom)
    {
        if (CM_Locate_DevNodeW(out var node, instanceId, 0) != 0 &&
            (!includePhantom || CM_Locate_DevNodeW(out node, instanceId, 1) != 0))
        {
            return false;
        }
        return IsVirtualDualSenseDeviceNode(node);
    }

    private static bool IsVirtualDualSenseDeviceNode(uint node)
    {
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
        Capture = 1,
    }

    private sealed record DefaultEndpointChange(
        DataFlow Flow, AudioRole Role, string EndpointId);

    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    private sealed class EndpointNotificationClient : IMMNotificationClient
    {
        private readonly Action<DataFlow, AudioRole, string?> _onDefaultDeviceChanged;

        internal EndpointNotificationClient(Action<DataFlow, AudioRole, string?> onDefaultDeviceChanged)
        {
            _onDefaultDeviceChanged = onDefaultDeviceChanged;
        }

        public int OnDeviceStateChanged(string deviceId, uint newState) => 0;
        public int OnDeviceAdded(string deviceId) => 0;
        public int OnDeviceRemoved(string deviceId) => 0;

        public int OnDefaultDeviceChanged(DataFlow flow, AudioRole role, string? defaultDeviceId)
        {
            _onDefaultDeviceChanged(flow, role, defaultDeviceId);
            return 0;
        }

        public int OnPropertyValueChanged(string deviceId, PropertyKey propertyKey) => 0;
    }

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct PropertyKey
    {
        private readonly Guid _formatId;
        private readonly uint _propertyId;
    }

    [ComImport]
    [Guid("7991EEC9-7E89-4D85-8390-6C703CEC60C0")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMNotificationClient
    {
        [PreserveSig]
        int OnDeviceStateChanged([MarshalAs(UnmanagedType.LPWStr)] string deviceId, uint newState);
        [PreserveSig]
        int OnDeviceAdded([MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        [PreserveSig]
        int OnDeviceRemoved([MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        [PreserveSig]
        int OnDefaultDeviceChanged(DataFlow flow, AudioRole role,
                                   [MarshalAs(UnmanagedType.LPWStr)] string? defaultDeviceId);
        [PreserveSig]
        int OnPropertyValueChanged([MarshalAs(UnmanagedType.LPWStr)] string deviceId,
                                   PropertyKey propertyKey);
    }

    [ComImport]
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceEnumerator
    {
        [PreserveSig]
        int EnumAudioEndpoints(DataFlow dataFlow, uint stateMask, out IMMDeviceCollection devices);
        [PreserveSig]
        int GetDefaultAudioEndpoint(DataFlow dataFlow, AudioRole role, out IMMDevice endpoint);
        [PreserveSig]
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice endpoint);
        [PreserveSig]
        int RegisterEndpointNotificationCallback(IMMNotificationClient callback);
        [PreserveSig]
        int UnregisterEndpointNotificationCallback(IMMNotificationClient callback);
    }

    [ComImport]
    [Guid("0BD7A1BE-7A1A-44DB-8397-C0A8FE7AF53E")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceCollection
    {
        [PreserveSig]
        int GetCount(out uint count);
        [PreserveSig]
        int Item(uint index, out IMMDevice endpoint);
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
