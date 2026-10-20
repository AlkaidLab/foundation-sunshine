using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Channels;
using Microsoft.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.Media.Audio;
using AudioRole = Windows.Win32.Media.Audio.ERole;

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

    internal readonly record struct DeviceNodeIdentity(string InstanceId, IReadOnlyList<string> HardwareIds);

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

    /// <summary>Starts an asynchronous monitor without blocking controller creation.</summary>
    internal DefaultAudioEndpointGuard(Action<AudioRole> onViolation)
    {
        _onViolation = onViolation;
        _worker = Task.Run(MonitorAsync);
    }

    /// <summary>Registers notifications before the initial snapshot and unregisters before releasing COM references.</summary>
    private async Task MonitorAsync()
    {
        IMMDeviceEnumerator? enumerator = null;
        EndpointNotificationClient? notification = null;
        var registered = false;
        try
        {
            enumerator = CoreAudio.CreateEnumerator();
            notification = new EndpointNotificationClient(OnDefaultDeviceChanged);
            var registrationResult = enumerator.RegisterEndpointNotificationCallback(notification);
            registered = registrationResult.Succeeded;

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
                    $"Unable to register the default audio endpoint monitor ({registrationResult}); " +
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
                if (result.Failed && !_stopping.IsCancellationRequested)
                    Console.Error.WriteLine($"Unable to unregister the default audio endpoint monitor: {result}");
            }
            CoreAudio.Release(enumerator);
            GC.KeepAlive(notification);
        }
    }

    /// <summary>Checks both flows and all three valid roles, excluding SDK enumeration sentinels.</summary>
    private void CheckCurrentDefaults(IMMDeviceEnumerator enumerator)
    {
        foreach (var flow in new[] { EDataFlow.eRender, EDataFlow.eCapture })
        {
            foreach (var role in new[] { AudioRole.eConsole, AudioRole.eMultimedia, AudioRole.eCommunications })
            {
                if (TryGetDefaultEndpointId(enumerator, flow, role, out var endpointId))
                    ReportIfVirtualDualSense(role, endpointId);
            }
        }
    }

    /// <summary>Uses low-frequency snapshots only when notification registration is unavailable.</summary>
    private async Task PollDefaultsAsync(IMMDeviceEnumerator enumerator)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(2));
        while (await timer.WaitForNextTickAsync(_stopping.Token))
            CheckCurrentDefaults(enumerator);
    }

    /// <summary>Consumes coalesced notifications outside the COM callback and isolates inspection failures.</summary>
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

    /// <summary>Stores the latest identity per flow and role without blocking the audio service callback.</summary>
    private void OnDefaultDeviceChanged(EDataFlow flow, AudioRole role, string? endpointId)
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

    /// <summary>Rejects invalid flow and role values before indexing the bounded notification slots.</summary>
    private static bool TryGetEndpointSlot(EDataFlow flow, AudioRole role, out int slot)
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

    /// <summary>Reports a matching virtual endpoint at most once for this controller session.</summary>
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

    /// <summary>Treats endpoints disappearing during a lookup as absent and releases the lookup reference.</summary>
    private static bool TryGetDefaultEndpointId(
        IMMDeviceEnumerator enumerator, EDataFlow flow, AudioRole role, out string endpointId)
    {
        endpointId = string.Empty;
        IMMDevice? endpoint = null;
        try
        {
            // AUDCLNT_E_DEVICE_INVALIDATED and E_NOTFOUND are normal while an
            // endpoint is being created or removed, so treat any failed lookup
            // as "not currently default". A later notification (or the rare
            // registration-failure polling fallback) will retry it.
            if (enumerator.GetDefaultAudioEndpoint(flow, role, out endpoint).Failed || endpoint is null)
                return false;
            endpointId = CoreAudio.GetEndpointId(endpoint);
            return true;
        }
        catch (COMException)
        {
            return false;
        }
        finally
        {
            CoreAudio.Release(endpoint);
        }
    }

    /// <summary>Resolves an MMDevice identity through its PnP ancestry instead of relying on its display name.</summary>
    internal static bool IsVirtualDualSenseEndpoint(string endpointId)
    {
        var instanceId = endpointId.StartsWith("SWD\\MMDEVAPI\\", StringComparison.OrdinalIgnoreCase)
            ? endpointId
            : "SWD\\MMDEVAPI\\" + endpointId;
        if (CM_Locate_DevNodeW(out var node, instanceId, 0) != 0)
            return false;

        return IsVirtualDualSenseDeviceNode(node);
    }

    /// <summary>Filters the current render snapshot to HIDMaestro-backed DualSense endpoints.</summary>
    internal static IReadOnlyList<string> GetActiveVirtualDualSenseRenderEndpoints() =>
        CoreAudio.GetActiveEndpointIds(EDataFlow.eRender).Where(IsVirtualDualSenseEndpoint).ToArray();

    /// <summary>Binds creation to one new render endpoint, rejecting stale or ambiguous identities.</summary>
    internal static string WaitForNewRenderEndpoint(
        IReadOnlySet<string> previous, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        var deadline = DateTime.UtcNow + timeout;
        do
        {
            cancellationToken.ThrowIfCancellationRequested();
            var created = GetActiveVirtualDualSenseRenderEndpoints()
                .Where(endpoint => !previous.Contains(endpoint)).ToArray();
            if (created.Length > 1)
                throw new InvalidOperationException("The current controller has ambiguous render endpoints");
            if (created.Length == 1)
                return created[0];
            Thread.Sleep(50);
        }
        while (DateTime.UtcNow < deadline);
        throw new TimeoutException("The current controller did not create an active render endpoint");
    }

    /// <summary>Requires the retiring controller's endpoint to disappear before reusing its identity.</summary>
    internal static void WaitForRenderEndpointRemoval(string endpointId, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (CoreAudio.GetActiveEndpointIds(EDataFlow.eRender).Contains(endpointId, StringComparer.OrdinalIgnoreCase))
        {
            if (DateTime.UtcNow >= deadline)
                throw new TimeoutException("The retiring controller's render endpoint remains active");
            Thread.Sleep(50);
        }
    }

    /// <summary>Optionally includes phantom nodes while provisioning policy before enumeration.</summary>
    internal static bool IsVirtualDualSenseDeviceNode(string instanceId, bool includePhantom)
    {
        if (CM_Locate_DevNodeW(out var node, instanceId, 0) != 0 &&
            (!includePhantom || CM_Locate_DevNodeW(out node, instanceId, 1) != 0))
        {
            return false;
        }
        return IsVirtualDualSenseDeviceNode(node);
    }

    /// <summary>Bounds ancestry traversal and collects hardware identities for classification.</summary>
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

    /// <summary>Requires Sony identity plus a legacy owner marker or a synthetic serial under shared USB/IP.</summary>
    internal static bool IsVirtualDualSenseChain(IEnumerable<DeviceNodeIdentity> chain)
    {
        var sonyDualSense = false;
        var hidMaestro = false;
        var hidMaestroSerial = false;
        var sharedUsbip = false;
        foreach (var node in chain)
        {
            sonyDualSense |= Contains(node.InstanceId, "VID_054C&PID_0CE6") ||
                             node.HardwareIds.Any(id => Contains(id, "VID_054C&PID_0CE6"));
            hidMaestro |= node.HardwareIds.Any(id =>
                id.Equals("ROOT\\HIDMAESTRO_UDE", StringComparison.OrdinalIgnoreCase) ||
                id.StartsWith("ROOT\\HIDMAESTRO_UDE\\", StringComparison.OrdinalIgnoreCase));
            hidMaestroSerial |= node.InstanceId.StartsWith(
                @"USB\VID_054C&PID_0CE6\HM", StringComparison.OrdinalIgnoreCase);
            sharedUsbip |= node.HardwareIds.Any(id =>
                id.Equals(@"ROOT\USBIP_WIN2\UDE", StringComparison.OrdinalIgnoreCase));
        }
        // Checking both properties avoids rejecting a physical Sony controller
        // or some unrelated HIDMaestro virtual device.
        // Managed transport creation no longer stamps the shared host controller.
        // Match the SDK-generated USB serial plus its transport ancestor instead.
        return sonyDualSense && (hidMaestro || (hidMaestroSerial && sharedUsbip));
    }

    /// <summary>Matches PnP identifiers without depending on their letter casing.</summary>
    private static bool Contains(string value, string needle) =>
        value.Contains(needle, StringComparison.OrdinalIgnoreCase);

    /// <summary>Sizes the native identity buffer and treats removed nodes as unavailable.</summary>
    private static string? GetDeviceId(uint node)
    {
        if (CM_Get_Device_ID_Size(out var length, node, 0) != 0)
            return null;
        var buffer = new StringBuilder(checked((int)length + 1));
        return CM_Get_Device_IDW(node, buffer, length + 1, 0) == 0 ? buffer.ToString() : null;
    }

    /// <summary>Reads hardware identifiers without modifying PnP state; inaccessible nodes provide no evidence.</summary>
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

    /// <summary>Cancels and joins the monitor before disposing its cancellation source.</summary>
    public void Dispose()
    {
        _stopping.Cancel();
        try { _worker.GetAwaiter().GetResult(); }
        catch (OperationCanceledException) { }
        _stopping.Dispose();
    }

    private sealed record DefaultEndpointChange(
        EDataFlow Flow, AudioRole Role, string EndpointId);

    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    private sealed unsafe class EndpointNotificationClient : IMMNotificationClient
    {
        private readonly Action<EDataFlow, AudioRole, string?> _onDefaultDeviceChanged;

        /// <summary>Retains the managed sink for the lifetime of the native callback registration.</summary>
        internal EndpointNotificationClient(Action<EDataFlow, AudioRole, string?> onDefaultDeviceChanged)
        {
            _onDefaultDeviceChanged = onDefaultDeviceChanged;
        }

        /// <summary>Acknowledges state notifications; classification follows default-device changes.</summary>
        public HRESULT OnDeviceStateChanged(PCWSTR deviceId, DEVICE_STATE newState) => new(0);
        /// <summary>Acknowledges enumeration without doing work on the native callback thread.</summary>
        public HRESULT OnDeviceAdded(PCWSTR deviceId) => new(0);
        /// <summary>Acknowledges removal without accessing a disappearing device on the callback thread.</summary>
        public HRESULT OnDeviceRemoved(PCWSTR deviceId) => new(0);

        /// <summary>Copies the borrowed native identity before returning from the callback.</summary>
        public HRESULT OnDefaultDeviceChanged(EDataFlow flow, AudioRole role, PCWSTR defaultDeviceId)
        {
            _onDefaultDeviceChanged(flow, role, defaultDeviceId.Value == null ? null : defaultDeviceId.ToString());
            return new(0);
        }

        /// <summary>Acknowledges unrelated property updates without reconfiguring the device.</summary>
        public HRESULT OnPropertyValueChanged(PCWSTR deviceId, PROPERTYKEY propertyKey) => new(0);
    }

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    /// <summary>Locates a current or explicitly requested phantom PnP node without changing its state.</summary>
    private static extern uint CM_Locate_DevNodeW(out uint deviceInstance,
                                                  string deviceId,
                                                  uint flags);

    [DllImport("cfgmgr32.dll")]
    /// <summary>Reads one parent link during bounded ancestry traversal.</summary>
    private static extern uint CM_Get_Parent(out uint parentDeviceInstance,
                                             uint deviceInstance,
                                             uint flags);

    [DllImport("cfgmgr32.dll")]
    /// <summary>Determines the identity buffer size before reading a device node.</summary>
    private static extern uint CM_Get_Device_ID_Size(out uint length,
                                                     uint deviceInstance,
                                                     uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    /// <summary>Copies a PnP identity into the caller-provided Unicode buffer.</summary>
    private static extern uint CM_Get_Device_IDW(uint deviceInstance,
                                                 StringBuilder buffer,
                                                 uint bufferLength,
                                                 uint flags);
}
