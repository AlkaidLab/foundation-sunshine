# Paired object capture support

These files are used by the live plugin tap and its own-process tests, rather
than a separate recorder application. The six pinned callbacks copy each object
before submission and pair it with Process/lifecycle results. The assembler
preserves object identity, properties and owned PCM before the native MAT worker.
The retained IPC structures keep their existing sizes and offsets.

Build through [sonic-mat-live](../sonic-mat-live/README.md). The build supplies the
pinned external MinHook dependency; no vendored copy or automatic installation is
included. Tests run inside their own process and do not attach to audiodg.
