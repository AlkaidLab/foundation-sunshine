using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Win32;

namespace Foundation.DualSense;

// Shared by Core cleanup, the standalone repair command and its isolated tests.
internal static class DualSenseGameInputMapping
{
    internal const string Identity = "054C0CE600010005";
    internal const string Path = @"SYSTEM\CurrentControlSet\Control\GameInput\Devices\" + Identity;
    internal static readonly (string Key, string Value, int Old, int Correct)[] Changes =
    {
        ("RightThumbstickY", "AxisIndex", 3, 5), ("LeftTrigger", "AxisIndex", 4, 3),
        ("RightTrigger", "AxisIndex", 5, 4), ("View", "ButtonIndex", 6, 8),
        ("Menu", "ButtonIndex", 7, 9), ("LeftThumbstickButton", "ButtonIndex", 8, 10),
        ("RightThumbstickButton", "ButtonIndex", 9, 11),
        ("A", "ButtonIndex", 0, 1), ("B", "ButtonIndex", 1, 2), ("X", "ButtonIndex", 2, 0),
    };

    internal static bool IsTarget(string identity) => string.Equals(identity, Identity, StringComparison.OrdinalIgnoreCase);

    internal static string Classify(RegistryTree? tree)
    {
        if (tree == null) return "mapping_missing";
        if (tree.SameAs(Template(false, false))) return "legacy_mapping";
        if (tree.SameAs(Template(true, false))) return "axes_fixed";
        var navigation = Template(true, false);
        foreach (var change in Changes.Skip(3).Take(4))
            navigation.Set("Gamepad\\" + change.Key, change.Value, RegistryDatum.DWord(change.Correct));
        if (tree.SameAs(navigation)) return "navigation_fixed";
        if (tree.SameAs(Template(true, true))) return "corrected_mapping";
        return "custom_mapping";
    }

    internal static RegistryTree Template(bool axes, bool buttons)
    {
        var tree = new RegistryTree();
        tree.AddNode(""); tree.AddNode("Gamepad");
        void Axis(string name, int index, bool invert = false)
        {
            tree.Set("Gamepad\\" + name, "AxisIndex", RegistryDatum.DWord(index));
            if (invert) tree.Set("Gamepad\\" + name, "Invert", RegistryDatum.DWord(1));
        }
        Axis("LeftThumbstickX", 0); Axis("LeftThumbstickY", 1, true);
        Axis("RightThumbstickX", 2); Axis("RightThumbstickY", 3, true);
        Axis("LeftTrigger", 4); Axis("RightTrigger", 5);
        var names = new[] { "A", "B", "X", "Y", "LeftShoulder", "RightShoulder", "View", "Menu", "LeftThumbstickButton", "RightThumbstickButton" };
        for (int i = 0; i < names.Length; ++i) tree.Set("Gamepad\\" + names[i], "ButtonIndex", RegistryDatum.DWord(i));
        foreach (var direction in new[] { "Up", "Down", "Left", "Right" })
        {
            var key = "Gamepad\\DPad" + direction;
            tree.Set(key, "SwitchIndex", RegistryDatum.DWord(0));
            tree.Set(key, "SwitchPosition", new RegistryDatum(RegistryValueKind.String, direction));
            tree.Set(key, "IncludeAdjacent", RegistryDatum.DWord(1));
        }
        foreach (var change in Changes.Take(axes ? 3 : 0).Concat(buttons ? Changes.Skip(3) : Array.Empty<(string Key, string Value, int Old, int Correct)>()))
            tree.Set("Gamepad\\" + change.Key, change.Value, RegistryDatum.DWord(change.Correct));
        return tree;
    }
}

internal sealed record RegistryDatum(RegistryValueKind Kind, string Data)
{
    internal static RegistryDatum DWord(int value) => new(RegistryValueKind.DWord, value.ToString(CultureInfo.InvariantCulture));
    internal object Value() => Kind switch
    {
        RegistryValueKind.DWord => int.Parse(Data, CultureInfo.InvariantCulture),
        RegistryValueKind.QWord => long.Parse(Data, CultureInfo.InvariantCulture),
        RegistryValueKind.Binary or RegistryValueKind.None => Convert.FromBase64String(Data),
        RegistryValueKind.MultiString => JsonSerializer.Deserialize<string[]>(Data)!,
        RegistryValueKind.String or RegistryValueKind.ExpandString => Data,
        _ => throw new InvalidDataException("Unsupported registry value type"),
    };
}

internal sealed class RegistryTree
{
    public SortedDictionary<string, SortedDictionary<string, RegistryDatum>> Nodes { get; set; } = new(StringComparer.Ordinal);
    internal void AddNode(string path) => Nodes.TryAdd(path.ToUpperInvariant(), new(StringComparer.Ordinal));
    internal void Set(string path, string name, RegistryDatum value)
    {
        AddNode(path); Nodes[path.ToUpperInvariant()][name.ToUpperInvariant()] = value;
    }
    internal RegistryDatum Get(string path, string name) => Nodes[path.ToUpperInvariant()][name.ToUpperInvariant()];
    internal string Hash() => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(
        Nodes.OrderBy(n => n.Key, StringComparer.Ordinal).Select(n => new {
            Key = n.Key, Values = n.Value.OrderBy(v => v.Key, StringComparer.Ordinal).ToArray() })))));
    internal bool SameAs(RegistryTree other) => Hash() == other.Hash();
    internal RegistryTree Copy() => JsonSerializer.Deserialize<RegistryTree>(JsonSerializer.Serialize(this))!;

    internal static RegistryTree? Read(RegistryKey hive, string path)
    {
        using var root = hive.OpenSubKey(path);
        if (root == null) return null;
        var tree = new RegistryTree();
        int values = 0, size = 0;
        void Walk(RegistryKey key, string relative, int depth)
        {
            if (depth > 8 || tree.Nodes.Count >= 128) throw new InvalidDataException("Registry tree exceeds inspection limits");
            tree.AddNode(relative);
            foreach (var name in key.GetValueNames())
            {
                if (++values > 512) throw new InvalidDataException("Registry tree exceeds inspection limits");
                var kind = key.GetValueKind(name);
                var value = key.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames)
                    ?? throw new IOException("Registry changed while reading");
                var data = value switch
                {
                    byte[] b => Convert.ToBase64String(b), string[] a => JsonSerializer.Serialize(a),
                    int n => n.ToString(CultureInfo.InvariantCulture), long n => n.ToString(CultureInfo.InvariantCulture),
                    string s => s, _ => throw new InvalidDataException("Unsupported registry value"),
                };
                size += data.Length + name.Length;
                if (size > 512 * 1024) throw new InvalidDataException("Registry tree exceeds inspection limits");
                tree.Set(relative, name, new(kind, data));
            }
            foreach (var child in key.GetSubKeyNames())
            {
                using var sub = key.OpenSubKey(child) ?? throw new IOException("Registry changed while reading");
                Walk(sub, relative.Length == 0 ? child : relative + "\\" + child, depth + 1);
            }
        }
        Walk(root, "", 0);
        return tree;
    }

}
