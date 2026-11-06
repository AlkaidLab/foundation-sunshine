using System.Text.Json;
using System.Text.Json.Serialization;
using PayloadPolicy;

namespace Ds5PayloadAudit;

internal sealed class Summary
{
    public int Allow { get; set; }
    public int Warn { get; set; }
    public int Deny { get; set; }
    public int Unknown { get; set; }
    public int Missing { get; set; }
    public long Bytes { get; set; }
}

internal sealed class AuditReport
{
    public int Schema { get; set; } = 1;
    public string Tool { get; set; } = "ds5-payload-audit";
    public string GeneratedAt { get; set; } = "";
    public string? PolicyPath { get; set; }
    public string PayloadPolicy { get; set; } = "";
    public string Target { get; set; } = "";
    public List<string> Inputs { get; set; } = [];
    public Summary Summary { get; set; } = new();
    public List<Finding> Findings { get; set; } = [];
}

internal sealed record Options(List<string> Inputs, string? PolicyPath, string? ReportPath)
{
    /// <summary>Returns null when the caller asked for help.</summary>
    public static Options? Parse(string[] args)
    {
        var inputs = new List<string>();
        string? policy = null, report = null;
        for (var index = 0; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--policy": policy = Next(args, ref index); break;
                case "--report": report = Next(args, ref index); break;
                case "-h":
                case "--help": return null;
                default:
                    if (args[index].StartsWith('-'))
                        throw new ArgumentException($"unknown option '{args[index]}'");
                    inputs.Add(args[index]);
                    break;
            }
        }
        if (inputs.Count == 0)
            throw new ArgumentException("no artifact was given");
        return new Options(inputs, policy, report);
    }

    private static string Next(string[] args, ref int index) =>
        index + 1 < args.Length
            ? args[++index]
            : throw new ArgumentException($"option '{args[index]}' needs a value");
}

internal static class Program
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    private static int Main(string[] args)
    {
        try
        {
            var options = Options.Parse(args);
            if (options is null)
            {
                PrintUsage();
                return 0;
            }

            var policy = options.PolicyPath is null ? Policy.Empty : Policy.Load(options.PolicyPath);
            var findings = new List<Finding>();
            var inspector = new Inspector(policy);
            foreach (var input in options.Inputs)
                inspector.Inspect(input, findings);
            inspector.Complete(findings);

            var report = new AuditReport
            {
                GeneratedAt = DateTimeOffset.UtcNow.ToString("O"),
                PolicyPath = policy.Path,
                PayloadPolicy = policy.Document.PayloadPolicy,
                Target = policy.Document.Target,
                Inputs = options.Inputs,
                Summary = Summarize(findings),
                Findings = findings,
            };

            if (options.ReportPath is not null)
            {
                var full = Path.GetFullPath(options.ReportPath);
                var directory = Path.GetDirectoryName(full);
                if (!string.IsNullOrEmpty(directory))
                    Directory.CreateDirectory(directory);
                File.WriteAllText(full, JsonSerializer.Serialize(report, JsonOptions) + Environment.NewLine);
            }

            PrintSummary(report);
            return ExitCode(report.Summary);
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"ds5-payload-audit: {error.Message}");
            return 1;
        }
    }

    private static Summary Summarize(List<Finding> findings)
    {
        var summary = new Summary();
        foreach (var finding in findings)
        {
            summary.Bytes += finding.Size;
            switch (finding.Verdict)
            {
                case "allow": summary.Allow++; break;
                case "warn": summary.Warn++; break;
                case "deny": summary.Deny++; break;
                case "missing": summary.Missing++; break;
                default: summary.Unknown++; break;
            }
        }
        return summary;
    }

    private static int ExitCode(Summary summary) =>
        summary.Deny > 0 ? 3 : summary.Unknown > 0 || summary.Missing > 0 ? 2 : 0;

    private static void PrintSummary(AuditReport report)
    {
        Console.WriteLine(
            $"ds5-payload-audit: policy={Value(report.PayloadPolicy)} target={Value(report.Target)} inputs={report.Inputs.Count}");
        Console.WriteLine(
            $"  allow {report.Summary.Allow}  warn {report.Summary.Warn}  deny {report.Summary.Deny}  " +
            $"unknown {report.Summary.Unknown}  missing {report.Summary.Missing}");
        PrintGroup(report, "deny", "denied");
        PrintGroup(report, "unknown", "unknown");
        PrintGroup(report, "missing", "missing");
    }

    private static void PrintGroup(AuditReport report, string verdict, string title)
    {
        var matches = report.Findings.Where(finding => finding.Verdict == verdict).ToList();
        if (matches.Count == 0)
            return;

        Console.WriteLine($"  {title}:");
        foreach (var finding in matches.Take(200))
            Console.WriteLine($"    {finding.Name}  {finding.Size}  [{Value(finding.Group)}] {finding.Reason}");
        if (matches.Count > 200)
            Console.WriteLine($"    ... {matches.Count - 200} more");
    }

    private static string Value(string? value) => string.IsNullOrEmpty(value) ? "-" : value;

    private static void PrintUsage() => Console.WriteLine(
        """
        ds5-payload-audit <artifact...> [--policy <policy.json>] [--report <report.json>]

        Inspects a ZIP or a single PE/managed assembly. ZIP entries, nested archives
        and .NET embedded resources are classified against the payload policy; the
        inspected assembly is never loaded.

        Exit codes: 0 clean, 2 unknown or missing payload, 3 denied payload, 1 error.
        """);
}
