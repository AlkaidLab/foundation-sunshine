# EasyTier direct connection: implementation gates

This document records the verification needed for the Sunshine and VPlus direct-connection work. The existing shared-secret invitation in PR #1014 is a prototype and must not be treated as per-device authorization.

## Evidence and constraints

- EasyTier Secure Mode supports separate credentials and revocation. Its management RPC generates and stores credential secrets on the host; it is not an existing public-key-only registration interface. Credential storage and delivery therefore need an explicit security design. See [Secure Mode](https://easytier.cn/guide/network/secure-mode.html) and the [v2.7.1 credential RPC definitions](https://github.com/EasyTier/EasyTier/blob/v2.7.1/easytier-proto/proto/api_instance.proto).
- VPlus currently embeds EasyTier 2.6.4. [Issue #2489](https://github.com/EasyTier/EasyTier/issues/2489) documents a credential-identity bug when loading TOML; [PR #2451](https://github.com/EasyTier/EasyTier/pull/2451) contains the fix. A compatible Android JNI build containing that fix must be pinned, built and tested before relying on credentials.
- A Windows test of 2.6.4 and 2.7.1 showed `rpc_portal` in TOML was ignored. The default listener accepted unauthenticated local CLI access to the test configuration. The command-line loopback option constrained the address, but did not authenticate local users. An IP whitelist denying loopback blocked the test CLI but also prevents Sunshine from using the management RPC; it has not been validated as a working end-to-end solution.
- The Android app's no-TUN path avoids `VpnService` for that path, but a configuration flag or displayed peer status alone does not prove that all session traffic remains direct. If a direct route cannot be established and maintained, the session must fail closed.

## RPC diagnostic

On a test machine, run `tools/easytier-rpc-probe.ps1` with paths to an official, verified `easytier-core.exe` and matching `easytier-cli.exe`. Supply `-Mode toml`, `-Mode loopback`, or `-Mode deny`. The script starts a temporary no-TUN instance with no peer or transport listener and a disposable test secret; it reports listener and unauthenticated CLI behavior, then stops its processes and removes its temporary files. It refuses inherited `ET_*` variables.

An unanswered CLI request does not prove local-user isolation or working streaming. The diagnostic does not replace tests of network connectivity, version compatibility, process identity or the actual authorization boundary.

## Implementation order

1. Reproduce credential creation, connection, expiry, revocation and reconnection with a pinned upstream core and Android JNI. Verify private key storage, transport and deletion, plus a defensible Windows RPC boundary. Do not ship a shared-secret fallback as per-device access.
2. Implement short-lived, single-use enrollment bound to the intended Sunshine host and client, followed by individually revocable device authorization. Protect the host credential store and the Android credential with platform mechanisms. Review bootstrap authentication and replay resistance before exposing issuance.
3. Complete application-level no-TUN forwarding with per-instance lifecycle and port isolation. Verify the actual path before and during streaming and fail closed if it is relayed or unknown.
4. Test audio, video, input, network changes, authorization failure and revocation on a phone using mobile data. Confirm no VPN prompt and no relayed media. Record direct-connect failures as failures, including double symmetric NAT where there is no reachable IPv6 or mapped endpoint.
