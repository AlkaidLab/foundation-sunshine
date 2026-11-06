using System;

namespace Foundation.DualSense;

// The shared physical identity is never owned by a virtual controller.
internal static class DualSenseGameInputMapping
{
    internal const string Identity = "054C0CE600010005";
    internal static bool IsTarget(string identity) => string.Equals(identity, Identity, StringComparison.OrdinalIgnoreCase);
}
