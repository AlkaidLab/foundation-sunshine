using Microsoft.Win32;

namespace Sunshine.Ds5Sidecar;

/// <summary>
/// Applies Windows' documented never-default policy to HIDMaestro-backed
/// DualSense audio interfaces before Windows creates the MMDevice endpoint.
/// Active speaker properties are committed separately through Core Audio.
/// </summary>
internal static class DefaultAudioEndpointPolicy
{
    private const string AudioInterfaceClass =
        @"SYSTEM\CurrentControlSet\Control\DeviceClasses\{6994ad04-93ef-11d0-a3cc-00a0c9223196}";
    private const string EndpointAssociation =
        "{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},2";
    private const string NeverSetAsDefaultEndpoint =
        "{F3E80BEF-1723-4FF2-BCC4-7F83DC5E46D4},3";
    private const string AnyKsNodeType = "{00000000-0000-0000-0000-000000000000}";
    // FLOW_MASK_RENDER | FLOW_MASK_CAPTURE | every default-device role.
    private const int AllRolesAndFlows = 0x00000307;

    /// <summary>
    /// Finds the virtual DualSense KS audio interface and applies the endpoint
    /// policy. A short poll is needed because the USB audio child appears
    /// asynchronously after HIDMaestro creates the composite device.
    /// </summary>
    /// <returns>True when a matching interface was found.</returns>
    internal static bool EnsureNeverDefault(TimeSpan timeout, bool includePhantom, out bool changed)
    {
        var deadline = DateTime.UtcNow + timeout;
        do
        {
            if (EnsureOnce(includePhantom, out changed))
                return true;
            if (DateTime.UtcNow >= deadline)
                break;
            Thread.Sleep(50);
        }
        while (true);

        changed = false;
        return false;
    }

    private static bool EnsureOnce(bool includePhantom, out bool changed)
    {
        changed = false;
        var matched = false;
        using var audioInterfaces = Registry.LocalMachine.OpenSubKey(AudioInterfaceClass, writable: false);
        if (audioInterfaces is null)
            return false;

        foreach (var interfaceName in audioInterfaces.GetSubKeyNames())
        {
            var interfacePath = AudioInterfaceClass + "\\" + interfaceName;
            using var interfaceKey = Registry.LocalMachine.OpenSubKey(interfacePath, writable: false);
            if (interfaceKey?.GetValue("DeviceInstance") is not string deviceInstance ||
                !DefaultAudioEndpointGuard.IsVirtualDualSenseDeviceNode(deviceInstance, includePhantom))
            {
                continue;
            }

            foreach (var referenceName in interfaceKey.GetSubKeyNames())
            {
                var parametersPath = interfacePath + "\\" + referenceName + "\\Device Parameters";
                using var parameters = Registry.LocalMachine.OpenSubKey(parametersPath, writable: true);
                if (parameters is null)
                    continue;

                using var endpoint = parameters.CreateSubKey(@"EP\0", writable: true);
                if (endpoint is null)
                    continue;

                matched = true;
                if (NeedsUpdate(endpoint.GetValue(EndpointAssociation), endpoint.GetValue(NeverSetAsDefaultEndpoint)))
                {
                    endpoint.SetValue(EndpointAssociation, AnyKsNodeType, RegistryValueKind.String);
                    endpoint.SetValue(NeverSetAsDefaultEndpoint, AllRolesAndFlows, RegistryValueKind.DWord);
                    changed = true;
                }
            }
        }

        return matched;
    }

    internal static bool NeedsUpdate(object? association, object? policy)
    {
        return association is not string associationText ||
               !associationText.Equals(AnyKsNodeType, StringComparison.OrdinalIgnoreCase) ||
               policy is not int policyMask ||
               (policyMask & AllRolesAndFlows) != AllRolesAndFlows;
    }
}
