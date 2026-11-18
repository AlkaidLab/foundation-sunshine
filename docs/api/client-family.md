# Client family labels

The host stores a client family for each paired device so the Web UI can mark
the device as an official client (官方客户端) or a third-party client
(第三方客户端). The label is advisory. Pairing certificates remain the only
client identity, and this field never grants or removes access.

## What is recorded

For every paired client the host keeps:

| Field | Meaning |
| --- | --- |
| `family` | `moonlight-vplus-android`, `moonlight-vplus-desktop`, `moonlight-macos-enhanced`, `voidlink`, `moonlight-upstream`, or `unknown` |
| `official` | `true` for the three AlkaidLab clients, `false` for VoidLink and upstream Moonlight, `null` when the family is unknown |
| `version` | the product version, only when the request carried it; otherwise `null` |
| `os` | `android`, `ios`, `tvos`, `macos`, `windows`, `linux`, or `null` |
| `family_source` | the winning rule and the evidence keys behind it |
| `last_seen` | Unix seconds of the last observation |

The record is written at pairing and refreshed at every `/launch`, `/resume`
and RTSP `ANNOUNCE`. Paired `/serverinfo` polls may add version or OS
evidence but never move `last_seen`. A first packet from the voidlink-c pen
barrel-roll extension also refreshes it. The raw User-Agent, query string and
SDP text are not persisted; only the normalized result and the evidence key
names are.

## API

`GET /api/clients/list` keeps its existing shape and adds six fields to each
entry of `named_certs`:

```json
{
  "status": "true",
  "named_certs": [
    {
      "name": "Living room",
      "uuid": "4A9023F3-9957-7F20-0302-CF638C7DFC2C",
      "enabled": true,
      "streaming": false,
      "family": "moonlight-macos-enhanced",
      "official": true,
      "version": "1.3.9",
      "os": "macos",
      "family_source": "moonlight-macos-enhanced-user-agent:header.user-agent,header.user-agent(version)",
      "last_seen": 1762243200
    }
  ]
}
```

An unknown family is `"family": "unknown", "official": null`, with `null`
for `version`, `os` and `family_source` when those values are not known.
`last_seen` is `null` only when no observation has been recorded; pairing
records one, so a paired but unknown client normally has a timestamp. A
controller or UI that displays the third-party tip must treat `null` as "no
answer" and keep the neutral presentation.

## The client in a session

`GET /api/runtime/sessions` and the nvhttp session list name the client of a
running session. The family record of the paired device is copied into each
session so a reader needs no second request:

| Key | Meaning |
| --- | --- |
| `client_name` | the name sent with the request, else the name stored with the paired certificate that authenticated it, else `unknown` |
| `client_uuid` | the pairing UUID of that certificate |
| `client_family` | the `family` of the paired device (`unknown` when no rule matched) |
| `client_official` | the `official` tri-state of the paired device |
| `client_os`, `client_version` | the `os` and `version` of the paired device, `null` when unknown |

The standard GameStream `/launch` and `/resume` carry no device name; the
Foundation extension sends `clientname`. A request without one, or with
`unknown`, takes the paired name. The certificate that authenticated the TLS
connection is the identity, so the name is looked up by pairing UUID and not
read from the request.

The pairing request carries `clientname` (Foundation extension) or
`devicename` (standard exchange). The host takes `clientname`, then
`devicename`, then `Named Zako`. The shared Moonlight client code sends the
constant `devicename=roth` for every device; that value names nothing and is
ignored.

A client that pairs again with the certificate it already has is the same
device: the host keeps its entry and its UUID and takes the name and family of
the new pairing (a pairing that observed no family keeps the stored one).
Earlier hosts added a second entry with the same certificate for every
re-pairing; those entries stay until they are removed in the interface.

## Evidence and rules

The classifier reads only what the GameStream exchange already sends:

- HTTP query arguments and header fields from the pairing, `/launch` and
  `/resume` requests;
- RTSP `ANNOUNCE` SDP attributes, captured before the host applies defaults;
- the voidlink-c pen barrel-roll packet magic already handled by the input
  path.

Rules are ordered and the first complete match wins. Absence of a
family-specific marker produces `unknown`; it never produces a third-party
verdict.

| Rule | Evidence | Result |
| --- | --- | --- |
| `voidlink-pen-barrel-roll` | `input.penBarrelRoll` equals `1` | voidlink |
| `voidlink-user-agent` | User-Agent contains the `VoidLink` product token | voidlink |
| `moonlight-macos-enhanced-user-agent` | User-Agent contains `MoonlightEnhanced` | macOS Enhanced |
| `moonlight-vplus-android-user-agent` | User-Agent contains `VPlus` and `Android` | V+ Android |
| `moonlight-vplus-desktop-user-agent` | User-Agent contains `VPlus` | V+ desktop |
| `moonlight-vplus-android-pyrowave` | SDP has `x-ml-pyrowave.protocolVersion` | V+ Android |
| `moonlight-upstream-standard` | SDP has `x-nv-general.featureFlags` and no `x-ml-` attribute | upstream Moonlight |

The User-Agent cases use the product grammar in RFC 9110 section 10.1.5.
Version extraction needs the `product/version` form; an absent or unparsable
version stays `null`. A later observation with a valid version replaces the
stored one, so upgrading a client updates the display without re-pairing. A
weaker observation only refreshes `last_seen`; it does not downgrade a known
family.

The PyroWave rule is grounded in this repository's documents: PyroWave is the
matching Moonlight V+ Android experimental contract, and the client declares
`x-ml-pyrowave.*` in `ANNOUNCE`. The pen packet is documented in
`src/pen_barrel_roll.h` as the independent voidlink-c extension. The upstream
rule uses the fact that `x-ml-*` is the Foundation fork namespace; a raw SDP
with only the standard `x-nv-*` fields has the upstream signature. That is a
protocol signature, not an authentication result: a third-party clone that
sends only the standard fields is indistinguishable and gets the same label.

The host keeps parsing the RTSP 1.0 ANNOUNCE form (RFC 2326 section 10.3)
because that is what the GameStream exchange uses. RFC 7826 (RTSP 2.0)
obsoletes RFC 2326 and removes the RECORD and ANNOUNCE methods, so it does not
define a replacement for this wire field. The current-standard note is kept
here so the deliberate RTSP 1.0 dependency is visible.

## What is deliberately not inferred

- A device name, a hostname or a pairing UUID is not a product identity.
- An HTTP library token such as `okhttp` identifies a library, not a client.
- Foundation-only SDP features (`x-ml-general.featureFlags`, PCM/AC3 audio,
  DualSense extensions, HDR capabilities) are shared by more than one
  Foundation client. Without a product token they stay `unknown`; the host
  does not guess between V+ desktop and macOS Enhanced.
- An old or custom build that sends no product token and no family-specific
  extension stays `unknown` even when it is probably official.

## Privacy and logging

- No probe, cookie, timing measurement or network request was added. Nothing
  leaves the host.
- RFC 9110 section 10.1.5 warns that overly detailed User-Agent values raise
  fingerprinting risk. This change only reads a value the client already
  sends and stores the normalized family, version and OS.
- Only the normalized record is persisted. The raw evidence is discarded
  after each request.
- A family change logs the family, OS, version, rule source and a masked
  client id (the first eight characters of the UUID). It never logs the raw
  User-Agent, device name or full UUID.
- `official` is a display hint, not an authorization decision. A client can
  copy another client's request fields; that changes the label, not access.

## Verification and limits

- `client_family_unit_tests` covers every family, the unknown cases, the
  version upgrade and the weaker-evidence merge using
  `tests/fixtures/client_families.json`. The fixtures are documented examples,
  not captured traffic; each fixture names its source.
- Real request captures from the shipping clients were not available in the
  repository at implementation time. The exact User-Agent strings emitted by
  the clients are therefore **not verified**; they are documented examples.
- Windows, macOS and Android client runs are **not verified** by this change.
  Linux unit and host builds are reported with the delivery.

## Sources read (2026-11-03)

- RFC 9110, HTTP Semantics, section 10.1.5 User-Agent:
  <https://www.rfc-editor.org/rfc/rfc9110.html#section-10.1.5>.
- RFC 8866, SDP: Session Description Protocol:
  <https://www.rfc-editor.org/rfc/rfc8866.html>.
- RFC 2326, RTSP 1.0, section 10.3 ANNOUNCE:
  <https://www.rfc-editor.org/rfc/rfc2326.html#section-10.3>;
  RFC 7826, RTSP 2.0 (which obsoletes RFC 2326):
  <https://www.rfc-editor.org/rfc/rfc7826.html>.
- `README.md` lines 65-73: the official client list and VoidLink.
- `docs/pyrowave_sunshine_research.md` and
  `docs/pyrowave-client-contract.md`: the PyroWave client contract.
- `src/pen_barrel_roll.h`: the voidlink-c pen extension.
- `src/rtsp.cpp`: the raw SDP namespace split between `x-nv-*` and `x-ml-*`.

Source-index records in `docs/authority/source-index.tsv`:
`client-family-rfc9110-user-agent-20261103`,
`client-family-rfc8866-sdp-20261103`,
`client-family-rfc2326-rtsp-20261103`,
`client-family-rfc7826-rtsp2-20261103`.
