using System.Diagnostics;

namespace Foundation.Input;

// Compiled into both Core and Sidecar so report and sensor timestamps use
// the same host QPC epoch, even when device creation takes several seconds.
internal static class DualSenseClock
{
    internal static uint Now => Encode(Stopwatch.GetTimestamp(), Stopwatch.Frequency);

    internal static uint Encode(long ticks, long frequency)
    {
        const long rate = 3_000_000;
        // Split before multiplying to avoid overflowing after long uptimes.
        // Keep natural uint32 rollover, including the valid value zero.
        return unchecked((uint)(ticks / frequency * rate + ticks % frequency * rate / frequency));
    }
}
