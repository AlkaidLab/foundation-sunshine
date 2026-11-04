# ds5-payload: policy and audit

Single source of truth for **what the DualSense sidecar may ship**, plus the tool
that proves a built artifact matches it. Introduced for
[issue #1194](https://github.com/AlkaidLab/foundation-sunshine/issues/1194): the
release ZIP carried Microsoft SDK/WDK signing and catalog tooling with no
redistribution basis for this use, embedded inside HIDMaestro.Core.dll where a
file-name scan cannot see it.

## Files

| Path | Role |
| --- | --- |
| policy.json | Allowed groups, denied patterns, required resources, driver-package pins |
| Ds5PayloadAudit/ | Read-only auditor (ZIP, directory or single binary) |
| fixtures/issue-1194.json | Baseline hashes and counts recorded by the issue |

## Why the auditor never loads an assembly

Assembly.Load runs module initializers and dependency resolution on an artifact
that is, by definition, untrusted. The auditor therefore reads the CLI header,
walks the ManifestResource table with System.Reflection.Metadata, locates each
embedded resource through the PE resources directory and hashes it. Nested ZIPs
are unpacked in memory; nothing is executed, extracted to disk or trusted.

## Verdicts and exit codes

| Verdict | Meaning |
| --- | --- |
| allow | Matches an allowed group |
| warn | Matches a group that still needs an exact pin (today: .NET runtime files) |
| deny | Matches a denied rule; denied always wins over allow |
| unknown | Matches nothing; a new payload object must be reviewed, never silently accepted |
| missing | A required resource did not appear in any inspected assembly |

Exit codes: 0 clean, 2 unknown or missing, 3 denied, 1 tool error.
--fail-on-warn also fails on warn.

## Usage

~~~powershell
# Audit the release ZIP the way CI does
.\scripts\audit-ds5-payload.ps1 -Artifact .\artifacts\Sunshine.Ds5Sidecar.x64.zip -Report .\artifacts\ds5-payload-audit.json

# Inspect a single assembly while developing
dotnet run --project tools/ds5-payload/Ds5PayloadAudit -- tools/sunshine-ds5-sidecar/bin/Release/net10.0-windows10.0.26100.0/win-x64/HIDMaestro.Core.dll --policy tools/ds5-payload/policy.json --report .tmp/audit.json
~~~

## Baseline (measured 2026-11-03, component 1.4.3)

| Metric | Value |
| --- | --- |
| Embedded resources in HIDMaestro.Core.dll | 395 / 58.06 MiB (394 upstream + 1 injected contract) |
| Resources denied by this policy | **49 / 57.08 MiB** |
| Denied: Microsoft signing + catalog tool chain | 21 |
| Denied: arm64 payload (no shipped target) | 21 |
| Denied: usbip-win2 installers (unreachable) | 2 (45.14 MiB) |
| Denied: SteamVR payload (unreferenced) | 5 |
| Payload that survives the policy | 346 / 0.98 MiB |

The same 49 denials are found whether the input is the ZIP or the extracted DLL,
which is the property the issue asked for: the audit cannot be satisfied by
changing outer file names.

## Measured after the first trimmed build (M1)

| Step | Result |
| --- | --- |
| PayloadTool extract with policy | 345 of 394 upstream resources, 49 denied |
| Core rebuild | 61,417,984 B -> **1,556,480 B** |
| PayloadTool verify | identity and API unchanged, 346 resources match the policy |
| PayloadTool audit | denied 0, unknown 0, missing 0 |
| Sidecar probe against the trimmed Core | protocol 1, both profiles, all markers present |

The audit still returns 49 denials for the untrimmed upstream assembly, which is
the negative control: trimming is what makes the gate pass, not the gate itself.

## Payload stages

payload_policy names the stage the artifact is built for:

- legacy-v1 - shipped 1.4.2/1.4.3 payload. Fails this policy by design; the
  baseline above is what the audit reports.
- composite-only-v1 - tools, arm64 and installers are gone and the runtime no
  longer signs anything. The x64 driver files stay in the payload but there is
  no catalog, so FullDeploy fails closed. HID-only runs on the USB/IP profile
  instead, or through an already installed driver; it needs neither a signed
  catalog nor a fresh deployment.
- audited-hid-v1 - reserved for a prebuilt, signed driver package. This
  repository has no provider for it, so the build refuses the stage instead of
  producing a component that could not install anything.

The stage also decides what the artifact may claim. Two different questions are
kept apart on purpose:

- the probe's hid_only_available answers "can HID-only run on this machine right
  now" (driver present, or the USB/IP transport available);
- runtime.json's hid_only_driver_deployable answers "can this package install the
  UMDF driver", which is exactly payload_policy != composite-only-v1.

Conflating them would let a downloaded package promise a deployment it cannot
perform, or hide a mode the machine can already run.

The capability deliberately does not live in component-compatibility.json. The
Control Panel compares that contract for JSON equality against the copy compiled
into its own build, so adding a field there would make every new component
uninstallable until the Control Panel is rebuilt. Keeping the stage in runtime.json
and the probe makes the artifact additive: an older Control Panel ignores the new
fields and still installs, and the sidecar refuses HID-only with a clear message.

Every surviving resource is byte-identical to the upstream release; there is no
substitution path. A signed driver package would be a different stage with its own
provenance, and this repository does not provide one.

Changing policy.json **is** the authorisation change. A new group or a widened
pattern must be reviewed; the auditor's job is to make that review unavoidable.

The release workflow runs this audit after packaging and before the artifacts
are uploaded, and a non-zero exit fails the job. deny, unknown and missing block;
warn does not, so the runtime catch-all below can stay visible without stopping a
release.

## Known gap (closed in M1)

dotnet-runtime and dotnet-runtime-metadata currently match by extension, so they
report warn instead of allow. M1 replaces them with a manifest generated from the
restored runtime pack, at which point --fail-on-warn can gate releases.
