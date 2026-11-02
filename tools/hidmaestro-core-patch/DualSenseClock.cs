using System.Diagnostics;

namespace Foundation.Input;

/// <summary>Shares the host QPC epoch between Core report time and Sidecar sensor time.</summary>
internal static class DualSenseClock
{
    /// <summary>Reads host QPC time as a wrapping 32-bit DualSense timestamp at 3 MHz.</summary>
    internal static uint Now => Encode(Stopwatch.GetTimestamp(), Stopwatch.Frequency);

    /// <summary>Converts nonnegative QPC ticks at a positive frequency to 3 MHz, preserving uint32 rollover.</summary>
    internal static uint Encode(long ticks, long frequency)
    {
        const long rate = 3_000_000;
        // Split before multiplying to avoid overflowing after long uptimes.
        // Keep natural uint32 rollover, including the valid value zero.
        return unchecked((uint)(ticks / frequency * rate + ticks % frequency * rate / frequency));
    }
}
