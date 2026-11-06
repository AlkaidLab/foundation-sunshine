using System.Text.Json;
using Microsoft.Win32;
using Foundation.DualSense;

namespace Sunshine.Ds5Sidecar;

internal sealed record GameInputChange(string Key, string Value, int Before, int After);
internal sealed record GameInputResult(string State, string? BackupId, bool RestartRequired,
    IReadOnlyList<GameInputChange> Changes, string? Detail = null, RegistryTree? Mapping = null,
    IReadOnlyList<string>? Backups = null);

internal sealed class GameInputJournal
{
    public int Schema { get; set; } = 1;
    public string Identity { get; set; } = DualSenseGameInputMapping.Identity;
    public string Id { get; set; } = "";
    public string Status { get; set; } = "pending";
    public DateTimeOffset CreatedUtc { get; set; } = DateTimeOffset.UtcNow;
    public bool RecreatingMissing { get; set; }
    public RegistryTree Before { get; set; } = new();
    public RegistryTree After { get; set; } = new();
    public string BeforeHash { get; set; } = "";
    public string AfterHash { get; set; } = "";
    public GameInputChange[] Changes { get; set; } = Array.Empty<GameInputChange>();
}

// All paths and identities are fixed by the caller. Tests use a disposable HKCU hive.
internal sealed class GameInputRepair(RegistryKey hive, string directory, Action? guard = null,
    Action<int>? afterWrite = null, Action<string>? validateFile = null, Action<string>? secureFile = null)
{
    private const string Target = DualSenseGameInputMapping.Path;
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    internal GameInputResult Check()
    {
        try
        {
            var tree = RegistryTree.Read(hive, Target);
            var ids = BackupIds();
            var pending = ids.FirstOrDefault(id => Read(id).Status is "pending" or "restoring");
            return new(pending == null ? DualSenseGameInputMapping.Classify(tree) : "recovery_required",
                pending, pending != null, Array.Empty<GameInputChange>(),
                Detail: pending == null ? null : "An interrupted mapping operation needs backup recovery", Mapping: tree, Backups: ids);
        }
        catch (UnauthorizedAccessException e) { return new("access_denied", null, false, [], e.Message); }
        catch (Exception e) when (e is IOException or InvalidDataException or JsonException)
        { return new("inspection_failed", null, false, [], e.Message); }
    }

    internal GameInputResult Repair()
    {
        guard?.Invoke();
        var check = Check();
        if (check.State is not ("legacy_mapping" or "axes_fixed" or "navigation_fixed" or "corrected_mapping")) return check;
        if (check.State == "corrected_mapping") return check;
        var before = check.Mapping!;
        var after = DualSenseGameInputMapping.Template(true, true);
        var changes = DualSenseGameInputMapping.Changes
            .Where(c => before.Get("Gamepad\\" + c.Key, c.Value) != RegistryDatum.DWord(c.Correct))
            .Select(c => new GameInputChange("Gamepad\\" + c.Key, c.Value,
                (int)before.Get("Gamepad\\" + c.Key, c.Value).Value(), c.Correct)).ToArray();
        var record = new GameInputJournal { Id = Guid.NewGuid().ToString("N"), Before = before,
            After = after, BeforeHash = before.Hash(), AfterHash = after.Hash(), Changes = changes };
        Save(record, create: true); // durable backup and intent precede every registry write
        var writeAttempted = false;
        try
        {
            RequireTree(before);
            Apply(record, restoring: false, startingWrite: () => writeAttempted = true);
            record.Status = "committed"; Save(record);
            return new("repaired", record.Id, true, changes);
        }
        catch (Exception error)
        {
            if (!writeAttempted)
            {
                // We are still alive and know no write was attempted. Do not leave
                // a pending intent that would block an externally edited mapping.
                try { File.Delete(FilePath(record.Id)); }
                catch (Exception cleanup) { return new("recovery_required", record.Id, false, [], error.Message + "; " + cleanup.Message); }
                return new("repair_failed", null, false, [], error.Message);
            }
            // Keep the intent durable. Restore checks the complete allowed intermediate
            // tree before changing anything; external edits are never blindly overwritten.
            try { Restore(record.Id); }
            catch (Exception rollback) { return new("recovery_required", record.Id, true, changes, error.Message + "; " + rollback.Message); }
            return new("repair_failed", record.Id, true, changes, error.Message);
        }
    }

    internal GameInputResult Restore(string id)
    {
        guard?.Invoke();
        var record = Read(id);
        foreach (var other in BackupIds())
            if (!string.Equals(other, id, StringComparison.OrdinalIgnoreCase)
                && Read(other).Status is "pending" or "restoring")
                throw new IOException("Recover the interrupted mapping operation before restoring an older backup");
        var current = RegistryTree.Read(hive, Target);
        if (current == null || (record.Status == "restoring" && record.RecreatingMissing))
        {
            if (current != null) ValidatePartialRestore(record, current);
            record.RecreatingMissing = true;
            record.Status = "restoring"; Save(record);
            RestoreMissing(record, current);
        }
        else
        {
            ValidateIntermediate(record, current);
            if (current.SameAs(record.Before))
            {
                record.Status = "restored"; Save(record);
                return new("already_restored", id, false, []);
            }
            record.Status = "restoring"; Save(record);
            Apply(record, restoring: true);
        }
        RequireTree(record.Before);
        record.Status = "restored"; Save(record);
        return new("restored", id, true, record.Changes);
    }

    private static void ValidatePartialRestore(GameInputJournal record, RegistryTree current)
    {
        foreach (var node in current.Nodes)
        {
            if (!record.Before.Nodes.TryGetValue(node.Key, out var expected))
                throw new IOException("Missing-map restore conflicts with another program's key");
            foreach (var value in node.Value)
                if (!expected.TryGetValue(value.Key, out var datum) || datum != value.Value)
                    throw new IOException("Missing-map restore conflicts with another program's value");
        }
    }

    private void RestoreMissing(GameInputJournal record, RegistryTree? current)
    {
        var expected = current?.Copy();
        foreach (var node in record.Before.Nodes)
        {
            guard?.Invoke();
            RequireTree(expected);
            using var key = hive.CreateSubKey(Target + (node.Key.Length == 0 ? "" : "\\" + node.Key));
            expected ??= new RegistryTree();
            expected.AddNode(node.Key);
            foreach (var value in node.Value)
            {
                RequireTree(expected);
                key.SetValue(value.Key, value.Value.Value(), value.Value.Kind); key.Flush();
                expected.Set(node.Key, value.Key, value.Value);
            }
        }
    }

    private void Apply(GameInputJournal record, bool restoring, Action? startingWrite = null)
    {
        var current = RegistryTree.Read(hive, Target) ?? throw new IOException("Mapping disappeared; no partial mapping will be created");
        ValidateIntermediate(record, current);
        // Open all keys first; a missing key or denied write causes no field changes.
        var keys = new List<RegistryKey>();
        try
        {
            foreach (var change in record.Changes)
                keys.Add(hive.OpenSubKey(Target + "\\" + change.Key, writable: true)
                    ?? throw new IOException("Mapping changed before repair"));
            RequireTree(current);
            for (int i = 0; i < record.Changes.Length; ++i)
            {
                var c = record.Changes[i]; var desired = restoring ? c.Before : c.After;
                guard?.Invoke();
                RequireTree(current);
                if (current.Get(c.Key, c.Value) == RegistryDatum.DWord(desired)) continue;
                startingWrite?.Invoke();
                keys[i].SetValue(c.Value, desired, RegistryValueKind.DWord); keys[i].Flush();
                current.Set(c.Key, c.Value, RegistryDatum.DWord(desired));
                if (!restoring) afterWrite?.Invoke(i);
            }
            RequireTree(restoring ? record.Before : record.After);
        }
        finally { foreach (var key in keys) key.Dispose(); }
    }

    private static void ValidateIntermediate(GameInputJournal record, RegistryTree current)
    {
        var normalized = current.Copy();
        foreach (var c in record.Changes)
        {
            if (!normalized.Nodes.TryGetValue(c.Key.ToUpperInvariant(), out var node)
                || !node.TryGetValue(c.Value.ToUpperInvariant(), out var datum)
                || (datum != RegistryDatum.DWord(c.Before) && datum != RegistryDatum.DWord(c.After)))
                throw new IOException("Mapping conflict: a repaired field was changed by another program");
            normalized.Set(c.Key, c.Value, RegistryDatum.DWord(c.Before));
        }
        if (!normalized.SameAs(record.Before)) throw new IOException("Mapping conflict: other fields changed; restore refused");
    }

    private void RequireTree(RegistryTree? expected)
    {
        var actual = RegistryTree.Read(hive, Target);
        if (expected == null ? actual != null : actual == null || !actual.SameAs(expected))
            throw new IOException("Mapping changed concurrently; operation stopped");
    }

    private string[] BackupIds()
    {
        FileAttributes attributes;
        try { attributes = File.GetAttributes(directory); }
        catch (DirectoryNotFoundException) { return []; }
        catch (FileNotFoundException) { return []; }
        if ((attributes & FileAttributes.Directory) == 0 || (attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException("Invalid backup directory");
        var files = Directory.GetFiles(directory, "*.json");
        if (files.Length > 256) throw new IOException("Too many repair records; inspect the backup directory");
        return files.OrderByDescending(File.GetLastWriteTimeUtc).Select(System.IO.Path.GetFileNameWithoutExtension).Cast<string>().ToArray();
    }
    private string FilePath(string id)
    {
        if (!Guid.TryParseExact(id, "N", out _)) throw new InvalidDataException("Invalid backup ID");
        return System.IO.Path.Combine(directory, id + ".json");
    }
    private GameInputJournal Read(string id)
    {
        var path = FilePath(id);
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0 || new FileInfo(path).Length > 1024 * 1024)
            throw new InvalidDataException("Invalid repair record");
        validateFile?.Invoke(path);
        var record = JsonSerializer.Deserialize<GameInputJournal>(File.ReadAllText(path)) ?? throw new InvalidDataException("Invalid repair record");
        if (record.Schema != 1 || record.Identity != DualSenseGameInputMapping.Identity || record.Id != id
            || record.Status is not ("pending" or "committed" or "restoring" or "restored")
            || (record.RecreatingMissing && record.Status is not ("restoring" or "restored"))
            || record.Before.Hash() != record.BeforeHash || record.After.Hash() != record.AfterHash
            || DualSenseGameInputMapping.Classify(record.Before) is not ("legacy_mapping" or "axes_fixed" or "navigation_fixed")
            || !record.After.SameAs(DualSenseGameInputMapping.Template(true, true)))
            throw new InvalidDataException("Repair record does not match a supported DualSense migration");
        var expected = DualSenseGameInputMapping.Changes
            .Where(c => record.Before.Get("Gamepad\\" + c.Key, c.Value) != RegistryDatum.DWord(c.Correct))
            .Select(c => new GameInputChange("Gamepad\\" + c.Key, c.Value, c.Old, c.Correct));
        if (!record.Changes.SequenceEqual(expected)) throw new InvalidDataException("Invalid repair change list");
        return record;
    }
    private void Save(GameInputJournal record, bool create = false)
    {
        Directory.CreateDirectory(directory);
        var path = FilePath(record.Id); var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
            {
                JsonSerializer.Serialize(file, record, Json); file.Flush(flushToDisk: true);
            }
            secureFile?.Invoke(temporary);
            File.Move(temporary, path, overwrite: !create);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
