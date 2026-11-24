# Zenless Zone Zero GENERAL_DATA key

`zzz-magic.bin` holds the 29 key bytes that decode the game's
`ZenlessZoneZero_Data/Persistent/LocalStorage/GENERAL_DATA.bin` settings
container (metadata template "ImSleepin", game data version 3).

The bytes were decoded once (2026-11-11) from the public launcher metadata
repository CollapseLauncher/CollapseLauncher-ReleaseRepo (MIT License,
Copyright (c) 2022 neon-nyan), files `metadata/v3/stable/config_ZZZ*.json`:
the template payload is RSA-1024 (PKCS#1 v1.5) encrypted with that
repository's master key and Brotli-compressed, and the national, bilibili
and international presets carry the same 29-byte key. The container
transform this key unlocks was ported from Collapse Launcher (MIT License,
Copyright (c) 2022 neon-nyan); see the repository NOTICE file.

SHA-256 of zzz-magic.bin:
`1db185bc785c5817d4dbae0f6719342bb47e0fbeef71d0381536167ce72dae11`

A file named `zzz-magic.bin` in the host's Sparxie data directory replaces
this shipped key.
