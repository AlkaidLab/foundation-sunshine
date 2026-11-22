# Brotli decoder (vendored source drop)

Upstream: <https://github.com/google/brotli>, tag `v1.1.0`
(commit `ed738e842d2fbdf2d6459e39267a633c4a9b2f5d), MIT license, see
`LICENSE`.

Only the decoder is vendored: `common/` (what upstream builds into
libbrotlicommon) and `dec/` (libbrotlidec) plus the public headers they
need. The files are verbatim copies, moved from upstream's `c/<dir>` to
`<dir>` so the sources' own relative includes keep working. The encoder,
command-line tools, tests and research code are not vendored.

What it is for: the Web UI build emits each text asset exactly once, already
compressed with Brotli (RFC 7932). The host serves those bytes directly with
`Content-Encoding: br` per RFC 9110 content negotiation; a client that does
not accept Brotli receives an identity representation decoded on the fly from
the same file, so the uncompressed copy no longer ships at all.

Provenance and file hashes: `source-drop-manifest.json`.

