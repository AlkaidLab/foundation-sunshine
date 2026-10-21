using Windows.Win32.Media.Audio;

namespace Sunshine.Ds5Sidecar;

/// <summary>Preserves existing user defaults across creation of one virtual audio device.</summary>
internal sealed class DefaultAudioEndpointSnapshot
{
    private sealed record Selection(EDataFlow Flow, ERole Role, string EndpointId);
    private readonly List<Selection> _defaults = new();
    private readonly HashSet<string> _previousEndpoints = new(StringComparer.OrdinalIgnoreCase);

    internal static DefaultAudioEndpointSnapshot Capture()
    {
        var snapshot = new DefaultAudioEndpointSnapshot();
        var enumerator = CoreAudio.CreateEnumerator();
        try
        {
            foreach (var flow in new[] { EDataFlow.eRender, EDataFlow.eCapture })
            {
                snapshot._previousEndpoints.UnionWith(CoreAudio.GetActiveEndpointIds(flow));
                foreach (var role in new[] { ERole.eConsole, ERole.eMultimedia, ERole.eCommunications })
                    if (DefaultAudioEndpointGuard.TryGetDefaultEndpointId(enumerator, flow, role, out var id) &&
                        !DefaultAudioEndpointGuard.IsVirtualDualSenseEndpoint(id))
                        snapshot._defaults.Add(new(flow, role, id));
            }
        }
        finally { CoreAudio.Release(enumerator); }
        return snapshot;
    }

    /// <summary>Restores only a switch to a newly created virtual endpoint, while the old default is still active.</summary>
    internal void Restore(string ownedRenderEndpoint)
    {
        var ownedController = DefaultAudioEndpointGuard.GetEndpointControllerId(ownedRenderEndpoint);
        var enumerator = CoreAudio.CreateEnumerator();
        try
        {
            foreach (var selection in _defaults)
            {
                var active = CoreAudio.GetActiveEndpointIds(selection.Flow);
                if (!active.Contains(selection.EndpointId, StringComparer.OrdinalIgnoreCase) ||
                    !DefaultAudioEndpointGuard.TryGetDefaultEndpointId(enumerator, selection.Flow, selection.Role, out var current) ||
                    !ShouldRestore(selection.EndpointId, current, _previousEndpoints.Contains(current),
                        DefaultAudioEndpointGuard.IsVirtualDualSenseEndpoint(current)))
                    continue;
                if (!string.Equals(current, ownedRenderEndpoint, StringComparison.OrdinalIgnoreCase) &&
                    (ownedController is null || !string.Equals(ownedController,
                        DefaultAudioEndpointGuard.GetEndpointControllerId(current), StringComparison.OrdinalIgnoreCase)))
                    continue;
                using var policy = new AudioPolicyConfig();
                policy.SetDefaultEndpoint(selection.EndpointId, selection.Role);
                if (!DefaultAudioEndpointGuard.TryGetDefaultEndpointId(enumerator, selection.Flow, selection.Role, out var restored) ||
                    !string.Equals(restored, selection.EndpointId, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException($"Unable to restore the {selection.Flow}/{selection.Role} default audio endpoint");
                Console.Error.WriteLine($"Restored {selection.Flow}/{selection.Role} default audio endpoint: {current} -> {restored}");
            }
        }
        finally { CoreAudio.Release(enumerator); }
    }

    internal static bool ShouldRestore(string previousDefault, string current, bool existedBefore, bool virtualDualSense) =>
        !string.Equals(previousDefault, current, StringComparison.OrdinalIgnoreCase) && !existedBefore && virtualDualSense;
}
