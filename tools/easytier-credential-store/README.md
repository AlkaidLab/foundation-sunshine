# Disposable Windows atomic-file probe

`windows_atomic_probe.cpp` is a standalone research experiment, **not** a
Sunshine credential-store implementation. It accepts no destination argument
and uses only synthetic public-key-like data. It does not touch existing
configuration, credentials, EasyTier processes, or existing ACLs.

On Windows, it checks the final handle-resolved TEMP path and each ancestor
for reparse points, requires a fixed NTFS volume with persistent ACLs, holds
directory handles, and creates a randomly named private test directory. Its
directory and files receive an explicit protected DACL **at creation** for the
current process identity, SYSTEM and administrators. File operations are
limited to direct children of that directory. It records file identities and
deletes only those exact owned objects by handle; the directory itself is
deleted by handle and verified absent. A failure to verify an object causes
cleanup to stop and report the disposable path rather than recursively delete.

From the repository root, an example build using a locally installed UCRT64
toolchain is:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$probeExe = Join-Path $env:TEMP ('sunshine-et-atomic-probe-' + [guid]::NewGuid().ToString('N') + '.exe')
if (Test-Path -LiteralPath $probeExe) { throw 'Output path exists' }
& 'C:\msys64\ucrt64\bin\g++.exe' -std=c++23 -O1 -Wall -Wextra -D_WIN32_WINNT=0x0A00 tools/easytier-credential-store/windows_atomic_probe.cpp -ladvapi32 -lbcrypt -o $probeExe
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
& $probeExe
```

The compiled executable is outside the disposable test directory. Its cleanup
is separate from the probe's own cleanup and must follow the operator's policy.

The test-only replacement API has three results: `not_committed` means this
operation never attempted the OS rename, `committed_verified` means rename
returned success and the held file's identity/path/DACL were checked, and
`indeterminate` covers any OS rename failure or any failure after rename was
attempted. A `not_committed` result does not prove that an external actor left
the destination unchanged. After a rename attempt the probe does not blindly
delete the source handle, because the source may already be the published
target; cleanup checks recorded file IDs and stops if identity is uncertain.

On the tested machine the probe passed simulated short-write, flush and
replacement failures, a real destination sharing violation, a flushed
same-volume replacement, and an injected post-rename verification failure.
The latter returned `indeterminate` while the new content was present. It also
rejected a hard-linked destination and a disposable junction, and detected a
destination whose file ID was replaced before the final check. Two owned child
processes were force-terminated immediately before and after rename; the parent
observed the expected old and new test contents, respectively. The parent
passed only a verified test-directory handle and its own candidate-file handle
to each child, using a restricted inherited-handle list. The probe verified its
test directory was absent afterward. Creating a file symbolic link was rejected
by Windows with error 1314, so that specific test was skipped; the junction
test did exercise a real reparse point. A run fails if neither reparse test can
be performed. The program prints PASS/SKIP/FAIL per run.

The tested `SetFileInformationByHandle(FileRenameInfo)` call rejected a
non-null `RootDirectory` with error 87 on this machine. The experiment therefore
holds the source and validated ancestor directory handles, rechecks target ID,
closes its target observation handles, then uses the validated absolute
destination in a same-volume rename. **This is not an atomic expected-file-ID
comparison.** Another process with the same Windows identity or an
administrator can still race the final check; those principals are outside the
experimental DACL boundary. The probe verifies the created DACL's entries but
does not perform a separate-user access test. The short-write and flush errors
are injected control-flow failures, not actual hardware or kernel I/O faults.
The child-termination test is a process-crash experiment with the operating
system and filesystem still running; it is not a power-loss test. No test
establishes survival of power loss, storage-cache failure, durable directory
metadata or atomicity of multiple files. A production store needs
separate threat-model, filesystem, crash-recovery and authorization review.
