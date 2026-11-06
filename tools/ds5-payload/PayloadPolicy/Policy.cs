using System.Text.Json;
using System.Text.RegularExpressions;

namespace PayloadPolicy;

public enum Verdict
{
    Allow,
    Warn,
    Deny,
    Unknown,
    Missing,
}

public sealed class Rule
{
    public string Id { get; set; } = "";
    public string[] Match { get; set; } = [];
    public string Verdict { get; set; } = "allow";
    public string? Reason { get; set; }
    public string? Provenance { get; set; }
    public string? License { get; set; }
}

public sealed class PolicyDocument
{
    public int Schema { get; set; }
    public string PayloadPolicy { get; set; } = "";
    public string Target { get; set; } = "";
    public List<Rule> Denied { get; set; } = [];
    public List<Rule> Groups { get; set; } = [];
    public List<string> Required { get; set; } = [];
}

public sealed class Policy
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    private readonly Dictionary<string, Regex> _globs = new(StringComparer.OrdinalIgnoreCase);

    private Policy(PolicyDocument document, string? path)
    {
        Document = document;
        Path = path;
    }

    public PolicyDocument Document { get; }

    public string? Path { get; }

    public static Policy Empty { get; } = new(new PolicyDocument(), null);

    public static Policy Load(string path)
    {
        var text = File.ReadAllText(path);
        var document = JsonSerializer.Deserialize<PolicyDocument>(text, JsonOptions)
                       ?? throw new InvalidDataException($"policy '{path}' is empty");
        if (document.Groups.Count == 0 || document.Denied.Count == 0)
            throw new InvalidDataException($"policy '{path}' has no groups or no denied rules");
        if (string.IsNullOrWhiteSpace(document.PayloadPolicy))
            throw new InvalidDataException($"policy '{path}' does not name a payload_policy stage");
        foreach (var rule in document.Denied.Concat(document.Groups))
        {
            if (rule.Match.Length == 0)
                throw new InvalidDataException($"policy rule '{rule.Id}' has no match patterns");
            if (rule.Id.Length == 0)
                throw new InvalidDataException("policy rule without an id");
        }
        return new Policy(document, path);
    }

    /// <summary>Denied rules win over allow groups, so a later rule can never
    /// re-admit a tool the policy forbids.</summary>
    public Verdict Classify(string name, out Rule? rule, out string? reason)
    {
        foreach (var denied in Document.Denied)
            if (Matches(denied, name)) { rule = denied; reason = denied.Reason; return Verdict.Deny; }

        foreach (var group in Document.Groups)
            if (Matches(group, name))
            {
                rule = group;
                reason = group.Reason;
                return group.Verdict.ToLowerInvariant() switch
                {
                    "allow" => Verdict.Allow,
                    "warn" => Verdict.Warn,
                    "deny" => Verdict.Deny,
                    _ => throw new InvalidDataException($"rule '{group.Id}' has an unknown verdict '{group.Verdict}'"),
                };
            }

        rule = null;
        reason = null;
        return Verdict.Unknown;
    }

    private bool Matches(Rule rule, string name)
    {
        foreach (var pattern in rule.Match)
            if (Match(pattern, name)) return true;
        return false;
    }

    private bool Match(string pattern, string name)
    {
        if (pattern.IndexOf('*') < 0 && pattern.IndexOf('?') < 0)
            return string.Equals(pattern, name, StringComparison.OrdinalIgnoreCase);

        if (!_globs.TryGetValue(pattern, out var regex))
        {
            var expression = "^" + Regex.Escape(pattern).Replace(@"\*", ".*").Replace(@"\?", ".") + "$";
            regex = new Regex(expression, RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.Singleline);
            _globs[pattern] = regex;
        }
        return regex.IsMatch(name);
    }
}
