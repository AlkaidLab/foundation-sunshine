using System.Text;

namespace Sparxie.Launch;

/// <summary>
/// Privacy pass over the bytes of a minidump before it leaves the computer (the same rules as the host's crash_core.cpp redact_private_names).
/// The dump type already keeps heap and string data out (CrashReport.TryDump: stack pointers only, module paths filtered); what can be left is
/// a path (the module list, a stack word that points at a string). Blanked in place with 'x', same length so the file stays valid, in ASCII/UTF-8
/// and in UTF-16LE: the account name behind a \Users\, /Users/ or /home/ marker (up to the next separator), and every whole-token occurrence
/// (not inside a longer word) of the names given (account, computer), ignoring ASCII case. Names under 3 bytes are ignored.
/// </summary>
internal static class DumpPrivacy
{
    private static byte[] Utf16(string text) => Encoding.Unicode.GetBytes(text);

    private static byte Fold(byte b) => b is >= (byte)'A' and <= (byte)'Z' ? (byte)(b + 32) : b;

    private static bool MatchesAt(byte[] data, int at, byte[] pattern)
    {
        if (at + pattern.Length > data.Length) return false;
        for (var k = 0; k < pattern.Length; k++)
            if (Fold(data[at + k]) != Fold(pattern[k])) return false;
        return true;
    }

    private static bool PathStop(int unit) => unit < 0x20 || unit == 0x7f || unit is '\\' or '/' or ':' or '*' or '?' or '"' or '<' or '>' or '|';

    private static bool WordUnit(byte[] data, int at, int unit)
    {
        if (at < 0 || at + unit > data.Length || (unit == 2 && data[at + 1] != 0)) return false;
        var c = data[at];
        return c is >= (byte)'0' and <= (byte)'9' or >= (byte)'a' and <= (byte)'z' or >= (byte)'A' and <= (byte)'Z' or (byte)'_';
    }

    private static void Blank(byte[] data, int from, int units, int unit)
    {
        for (var k = 0; k < units; k++)
        {
            data[from + k * unit] = (byte)'x';
            if (unit == 2) data[from + k * unit + 1] = 0;
        }
    }

    private static int BlankAfterMarker(byte[] data, byte[] marker, int unit)
    {
        var hits = 0;
        for (var i = 0; i + marker.Length < data.Length; i++)
        {
            if (!MatchesAt(data, i, marker)) continue;
            var start = i + marker.Length;
            var units = 0;
            var allX = true;
            for (var j = start; units < 64 && j + unit <= data.Length; j += unit)
            {
                var u = unit == 2 ? data[j] | (data[j + 1] << 8) : data[j];
                if (PathStop(u)) break;
                if (u != 'x') allX = false;
                units++;
            }
            if (units > 0 && !allX)
            {
                Blank(data, start, units, unit);
                hits++;
            }
            i = start + units * unit - 1;
        }
        return hits;
    }

    private static int BlankToken(byte[] data, byte[] pattern, int unit)
    {
        var hits = 0;
        var first = Fold(pattern[0]);
        for (var i = 0; i + pattern.Length <= data.Length; i++)
        {
            if (Fold(data[i]) != first || !MatchesAt(data, i, pattern)) continue;
            if (i >= unit && WordUnit(data, i - unit, unit)) continue;
            if (WordUnit(data, i + pattern.Length, unit)) continue;
            Blank(data, i, pattern.Length / unit, unit);
            hits++;
            i += pattern.Length - 1;
        }
        return hits;
    }

    /// <summary>Blanks account and computer names in <paramref name="data"/>; returns how many places were blanked.</summary>
    internal static int Redact(byte[] data, IEnumerable<string> names)
    {
        var hits = 0;
        foreach (var marker in new[] { "\\Users\\", "/Users/", "/home/" })
        {
            hits += BlankAfterMarker(data, Encoding.ASCII.GetBytes(marker), 1);
            hits += BlankAfterMarker(data, Utf16(marker), 2);
        }
        foreach (var name in names)
        {
            if (Encoding.UTF8.GetByteCount(name) < 3) continue;
            hits += BlankToken(data, Encoding.UTF8.GetBytes(name), 1);
            hits += BlankToken(data, Utf16(name), 2);
        }
        return hits;
    }

    /// <summary>What names this account and this computer (environment; nothing is read from the person's files).</summary>
    internal static List<string> PrivateNames()
    {
        var names = new List<string>();
        foreach (var value in new[] { Environment.UserName, Environment.MachineName, Environment.GetEnvironmentVariable("USERNAME"), Environment.GetEnvironmentVariable("COMPUTERNAME") })
            if (!string.IsNullOrEmpty(value) && !value.Equals("system", StringComparison.OrdinalIgnoreCase) && !names.Contains(value, StringComparer.OrdinalIgnoreCase)) names.Add(value);
        return names;
    }
}
