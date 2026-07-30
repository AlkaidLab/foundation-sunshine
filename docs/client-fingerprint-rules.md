# Client fingerprint rule feed

Sunshine always ships a conservative built-in warning rule. An optional signed
feed can add, replace, or disable warning rules without changing authentication
or connection authorization.

Remote rules are deliberately limited:

- only the `warn` action and `high` confidence are accepted;
- supported predicates are `present`, `equals`, `not_equal`, `one_of`, and
  `prefix`;
- scripts and regular expressions are not supported;
- document, rule, predicate, identifier, and value counts are bounded;
- expired, unsigned, invalidly signed, or rolled-back feeds are rejected.

The desktop GUI is only the transport. It periodically downloads the envelope
and submits it to the Core over the local tray API. The C++ Core remains the
trust boundary: it verifies the signature and schema, enforces expiry and
monotonic revisions, writes the last-known-good cache, and evaluates rules.
Replacing or modifying the GUI therefore does not permit unsigned rules to be
activated.

## Source payload

Maintain the readable payload in a dedicated, reviewed rule repository:

```json
{
  "schema_version": 1,
  "revision": 2,
  "issued_at": 1785369600,
  "expires_at": 1793145600,
  "rules": [
    {
      "id": "axixi-moonlight-android-2026-07",
      "enabled": true,
      "confidence": "high",
      "action": "warn",
      "message_key": "suspected_unknown_infringing_client",
      "all": [
        { "query": "virtualDisplay", "op": "present" },
        { "query": "virtualDisplay", "op": "not_equal", "value": "0" },
        { "query": "virtualDisplayMode", "op": "present" },
        { "query": "devicenickname", "op": "present" },
        { "query": "ppi", "op": "present" },
        { "query": "screen_resolution", "op": "present" },
        { "query": "timeToTerminateApp", "op": "equals", "value": "-1" },
        { "query": "UIScale", "op": "equals", "value": "200" }
      ]
    }
  ]
}
```

An entry with `"enabled": false` is a signed tombstone that disables a rule
with the same ID, including a built-in rule.

## Published envelope

CI signs the exact payload bytes with SHA-256 and the private key corresponding
to the configured X.509 certificate. It then publishes:

```json
{
  "payload": "<base64 of the exact payload bytes>",
  "signature": "<base64 of the detached signature>"
}
```

Each revision must be immutable and strictly greater than the previous
published revision. Keep the signing private key outside both the Sunshine and
rule repositories.

Configure Sunshine with:

```ini
client_fingerprint_remote_rules = enabled
client_fingerprint_rules_url = https://example.invalid/client-fingerprint-rules/stable.json
client_fingerprint_rules_certificate = /path/to/client-fingerprint-rules.pem
client_fingerprint_rules_refresh_hours = 24
```

The GUI accepts only an absolute HTTPS URL, does not follow redirects, uses
ETag revalidation, and limits the fully decoded response to 512 KiB. An empty
URL disables network access while retaining the built-in and cached rules.
The refresh interval is clamped to 1–168 hours.

The verified last-known-good envelope is cached as
`client-fingerprint-rules.json` in Sunshine's application data directory.
