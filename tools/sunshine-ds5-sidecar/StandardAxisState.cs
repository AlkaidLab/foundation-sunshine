using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal sealed class StandardAxisState
{
    private readonly (HMAxis Axis, int Source)[] _bindings;

    internal StandardAxisState(HMProfile profile)
    {
        // Ask the SDK for the mapping once, with a distinct marker for each
        // source. This preserves its canonical-axis fallbacks and overwrite
        // order without rebuilding the profile's sticks/triggers every frame.
        _bindings = HMGamepadStateHelpers.StandardAxes(profile, 0, 1, 2, 3, 4, 5)
            .Select(pair => (pair.Key, (int)pair.Value)).ToArray();
        Values = HMGamepadStateHelpers.StandardAxes(profile);
    }

    internal Dictionary<HMAxis, float> Values { get; }

    internal void Update(float lx, float ly, float rx, float ry, float lt, float rt)
    {
        ReadOnlySpan<float> values = stackalloc float[] { lx, ly, rx, ry, lt, rt };
        foreach (var (axis, source) in _bindings)
            Values[axis] = values[source];
    }
}
