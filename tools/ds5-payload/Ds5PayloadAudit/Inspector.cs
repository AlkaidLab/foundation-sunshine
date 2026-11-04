using System.Buffers.Binary;
using System.IO.Compression;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Security.Cryptography;
using PayloadPolicy;

namespace Ds5PayloadAudit;

internal sealed class Finding
{
    public string Input { get; set; } = "";
    public string Container { get; set; } = "";
    public string Name { get; set; } = "";
    public string Kind { get; set; } = "";
    public long Size { get; set; }
    public string Sha256 { get; set; } = "";
    public string Verdict { get; set; } = "unknown";
    public string? Group { get; set; }
    public string? Reason { get; set; }
}

/// <summary>Walks a directory, a ZIP, or a single binary and classifies every
/// payload object against the policy. Nested ZIPs and assembly resources are
/// included so a tool hidden inside HIDMaestro.Core.dll cannot pass review by
/// only looking at the outer archive's file names.</summary>
internal sealed class Inspector(Policy policy)
{
    private const long MaxBytes = 512L * 1024 * 1024;
    private const int MaxNesting = 3;
    private readonly HashSet<string> _seenResources = new(StringComparer.OrdinalIgnoreCase);

    public void Inspect(string path, List<Finding> findings)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException($"artifact not found: {path}", path);
        InspectFile(path, Path.GetFileName(path), path, findings, isRoot: true);
    }

    /// <summary>Required resources that never appeared in any inspected
    /// assembly are reported, so a trimmed payload cannot silently lose a file
    /// the runtime still needs.</summary>
    public void Complete(List<Finding> findings)
    {
        foreach (var name in policy.Document.Required)
            if (!_seenResources.Contains(name))
                findings.Add(new Finding
                {
                    Name = name,
                    Kind = "missing",
                    Verdict = "missing",
                    Reason = "required resource is absent from every inspected assembly",
                });
    }

    private void InspectFile(string fullPath, string displayName, string input, List<Finding> findings, bool isRoot = false)
    {
        var info = new FileInfo(fullPath);
        if (info.Length > MaxBytes)
        {
            findings.Add(Unreadable(input, "", displayName, "file", "file exceeds the inspector limit"));
            return;
        }

        byte[] bytes;
        try
        {
            bytes = File.ReadAllBytes(fullPath);
        }
        catch (Exception error)
        {
            findings.Add(Unreadable(input, "", displayName, "file", error.Message));
            return;
        }

        // A ZIP passed on the command line is the container under audit, not a
        // payload object. Its entries are the objects; nested archives stay
        // classified so a smuggled ZIP is still rejected. Content decides, so an
        // archive renamed to something innocuous is still opened.
        var isZip = displayName.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) || LooksLikeZip(bytes);
        var isContainer = isRoot && isZip;
        if (!isContainer)
            AddEntry(input, "", displayName, "file", bytes, findings,
                LooksExecutable(bytes) && !IsAssemblyName(displayName)
                    ? "executable content stored under a non-executable name"
                    : null);
        if (isZip)
            TryExpandZip(bytes, displayName, input, findings, depth: 1);
        else if (IsAssemblyName(displayName) || LooksManaged(bytes))
            TryAddResources(input, displayName, bytes, findings, depth: 0);
    }

    /// <summary>Expands an archive, recording a corrupt one instead of aborting
    /// the audit: a truncated archive must still appear in the report.</summary>
    private void TryExpandZip(byte[] bytes, string label, string input, List<Finding> findings, int depth)
    {
        try
        {
            ExpandZip(bytes, label, input, findings, depth);
        }
        catch (InvalidDataException error)
        {
            findings.Add(Unreadable(input, "", label, "zip", error.Message));
        }
    }

    private void ExpandZip(byte[] bytes, string zipName, string input, List<Finding> findings, int depth)
    {
        using var stream = new MemoryStream(bytes, writable: false);
        using var archive = new ZipArchive(stream, ZipArchiveMode.Read);
        foreach (var entry in archive.Entries)
        {
            var name = entry.FullName.Replace('\\', '/');
            if (name.EndsWith("/", StringComparison.Ordinal) || name.Length == 0)
                continue;
            if (entry.Length > MaxBytes)
            {
                findings.Add(Unreadable(input, zipName, name, "zip-entry", "entry exceeds the inspector limit"));
                continue;
            }

            byte[] content;
            try
            {
                using var source = entry.Open();
                using var buffer = new MemoryStream();
                source.CopyTo(buffer);
                content = buffer.ToArray();
            }
            catch (Exception error)
            {
                findings.Add(Unreadable(input, zipName, name, "zip-entry", error.Message));
                continue;
            }

            var isArchive = name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) || LooksLikeZip(content);
            var isAssembly = LooksManaged(content);
            var isExecutable = LooksExecutable(content);
            // A ZIP that cannot be expanded, or an assembly stored under a name
            // the policy allows as data, is not "allowed": it was not inspected.
            if (isArchive && depth >= MaxNesting)
                AddEntry(input, zipName, name, "zip-entry", content, findings,
                    $"archive nesting limit {MaxNesting} reached; contents were not inspected");
            else if (isExecutable && !IsAssemblyName(name))
                AddEntry(input, zipName, name, "zip-entry", content, findings,
                    "executable content stored under a non-executable name");
            else
                AddEntry(input, zipName, name, "zip-entry", content, findings);

            if (isArchive)
            {
                if (depth < MaxNesting)
                    TryExpandZip(content, $"{zipName}!{name}", input, findings, depth + 1);
            }
            else if (isAssembly || IsAssemblyName(name))
                TryAddResources(input, $"{zipName}!{name}", content, findings, depth);
        }
    }

    private void AddEntry(string input, string container, string name, string kind, byte[] bytes,
        List<Finding> findings, string? notInspected = null)
    {
        var verdict = policy.Classify(name, out var rule, out var reason);
        findings.Add(new Finding
        {
            Input = input,
            Container = container,
            Name = name,
            Kind = kind,
            Size = bytes.LongLength,
            Sha256 = Hash(bytes),
            // An object we refused to inspect is unknown even when its name would
            // otherwise match an allowed group.
            Verdict = notInspected is null ? Label(verdict) : "unknown",
            Group = notInspected is null ? rule?.Id : null,
            Reason = notInspected ?? reason,
        });
    }

    private void TryAddResources(string input, string container, byte[] bytes, List<Finding> findings, int depth)
    {
        List<(string Name, int Size, string Sha, byte[]? Archive, bool Executable)>? resources;
        try
        {
            resources = ManagedResources.Read(bytes);
        }
        catch (Exception error)
        {
            findings.Add(Unreadable(input, container, container, "assembly", $"unable to read managed resources: {error.Message}"));
            return;
        }
        if (resources is null)
            return;

        foreach (var resource in resources)
        {
            _seenResources.Add(resource.Name);
            var verdict = policy.Classify(resource.Name, out var rule, out var reason);
            // A resource that is executable but named like data is not allowed:
            // the name would let it pass review untouched.
            var notInspected = resource.Executable && !IsAssemblyName(resource.Name)
                ? "executable content stored under a non-executable name"
                : null;
            findings.Add(new Finding
            {
                Input = input,
                Container = container,
                Name = resource.Name,
                Kind = "assembly-resource",
                Size = resource.Size,
                Sha256 = resource.Sha,
                Verdict = notInspected is null ? Label(verdict) : "unknown",
                Group = notInspected is null ? rule?.Id : null,
                Reason = notInspected ?? reason,
            });

            // A resource can itself be an archive; it must not hide payload the
            // policy would reject. Too deep to expand means unknown, not allowed.
            if (resource.Archive is null)
                continue;
            if (depth >= MaxNesting)
            {
                findings.Add(Unreadable(input, container, resource.Name, "assembly-resource",
                    $"archive nesting limit {MaxNesting} reached; resource contents were not inspected"));
                continue;
            }
            TryExpandZip(resource.Archive, $"{container}!{resource.Name}", input, findings, depth + 1);
        }
    }

    private static Finding Unreadable(string input, string container, string name, string kind, string reason) => new()
    {
        Input = input,
        Container = container,
        Name = name,
        Kind = kind,
        Verdict = "unknown",
        Reason = reason,
    };

    private static bool LooksLikeZip(byte[] bytes) =>
        bytes.Length >= 4 && bytes[0] == 0x50 && bytes[1] == 0x4B &&
        ((bytes[2] == 0x03 && bytes[3] == 0x04) || (bytes[2] == 0x05 && bytes[3] == 0x06));

    /// <summary>True for any PE image, managed or native. Content decides whether
    /// a name may be trusted: an executable renamed to look like data is unknown.</summary>
    internal static bool LooksExecutable(byte[] bytes)
    {
        if (bytes.Length < 0x40 || bytes[0] != (byte)'M' || bytes[1] != (byte)'Z')
            return false;
        var offset = BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(0x3C, 4));
        return offset > 0 && (long)offset + 4 <= bytes.Length &&
               bytes[offset] == (byte)'P' && bytes[offset + 1] == (byte)'E' &&
               bytes[offset + 2] == 0 && bytes[offset + 3] == 0;
    }

    private static bool IsAssemblyName(string name) =>
        name.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) ||
        name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase);

    /// <summary>True for a PE image with a CLI header, whatever the file is
    /// called. A managed assembly must not hide behind a name the policy allows
    /// as data.</summary>
    private static bool LooksManaged(byte[] bytes)
    {
        if (bytes.Length < 0x40 || bytes[0] != (byte)'M' || bytes[1] != (byte)'Z')
            return false;
        try
        {
            using var stream = new MemoryStream(bytes, writable: false);
            using var pe = new PEReader(stream);
            return pe.HasMetadata;
        }
        catch
        {
            return false;
        }
    }

    private static string Label(Verdict verdict) => verdict switch
    {
        Verdict.Allow => "allow",
        Verdict.Warn => "warn",
        Verdict.Deny => "deny",
        Verdict.Missing => "missing",
        _ => "unknown",
    };

    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}

/// <summary>Reads the embedded-resource table without loading the assembly.
/// Resource bytes are located through the CLI header's resources directory and
/// hashed individually, which is what makes the 395-resource inventory
/// reviewable at all.</summary>
internal static class ManagedResources
{
    public static List<(string Name, int Size, string Sha, byte[]? Archive, bool Executable)>? Read(byte[] image)
    {
        using var stream = new MemoryStream(image, writable: false);
        using var pe = new PEReader(stream);
        if (!pe.HasMetadata)
            return null;

        var corHeader = pe.PEHeaders.CorHeader;
        if (corHeader is null)
            return null;

        var directory = corHeader.ResourcesDirectory;
        if (directory.RelativeVirtualAddress == 0 || directory.Size == 0)
            return [];

        var block = pe.GetSectionData(directory.RelativeVirtualAddress).GetContent();
        var span = block.AsSpan();
        var reader = pe.GetMetadataReader();
        var results = new List<(string Name, int Size, string Sha, byte[]? Archive, bool Executable)>();

        foreach (var handle in reader.ManifestResources)
        {
            var resource = reader.GetManifestResource(handle);
            if (!resource.Implementation.IsNil)
                continue;

            var rawOffset = resource.Offset;
            if (rawOffset < 0 || rawOffset + 4 > span.Length)
                throw new InvalidDataException($"resource '{reader.GetString(resource.Name)}' offset is out of range");

            var offset = checked((int)rawOffset);
            var length = BinaryPrimitives.ReadInt32LittleEndian(span.Slice(offset, 4));
            if (length < 0 || (long)offset + 4 + length > span.Length)
                throw new InvalidDataException($"resource '{reader.GetString(resource.Name)}' length is out of range");

            var data = span.Slice(offset + 4, length);
            // Keep archive resources so the caller can inspect their entries; a
            // plain resource stays hashed and is not copied.
            var archive = data.Length >= 4 && data[0] == 0x50 && data[1] == 0x4B && data[2] == 0x03 && data[3] == 0x04
                ? data.ToArray()
                : null;
            results.Add((reader.GetString(resource.Name), length,
                Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant(), archive,
                Inspector.LooksExecutable(data.ToArray())));
        }

        results.Sort(static (left, right) => string.CompareOrdinal(left.Name, right.Name));
        return results;
    }
}
