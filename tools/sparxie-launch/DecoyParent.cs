// The decoy-parent launch of Endfield.exe, ported from the Collapse Launcher Endfield plugin
// (Hi3Helper.Plugin.Endfield / Hi3Helper.Hypergryph.Core, MIT, Copyright (c) 2025 Collapse Launcher,
// Management/ParentProcessBridge.cs and GameDecoyParentBridge.cs at commit 1efd304; see the NOTICE file).
// The same method is implemented in C++ in Sparxie's EndfieldCore (native/EndfieldCore/endfield_decoy.cpp).
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Sparxie.Launch;

/// <summary>
/// A Hypergryph-signed image kept suspended as the direct parent of Endfield.exe. The client checks only its immediate parent, so the parent
/// never has to be a running launcher: it has to be a file named Games.exe with the game's signer. The suspended image is handed to CreateProcess as
/// PROC_THREAD_ATTRIBUTE_PARENT_PROCESS and is never resumed; it is ended and its session folder removed when this object is disposed.
/// </summary>
internal sealed class DecoyParent : IDisposable
{
    /// <summary>The file name the client's parent-process check accepts.</summary>
    public const string ImageName = "Games.exe";
    /// <summary>The default source image, in the game's folder (the plugin: GameDecoyParentBridge.Create).</summary>
    public const string DefaultSource = "PlatformProcess.exe";

    private const uint CreateSuspended = 0x00000004;
    private const uint CreateNoWindow = 0x08000000;
    private const uint ExtendedStartupInfoPresent = 0x00080000;
    private const nint ProcThreadAttributeParentProcess = 0x00020000;
    private static readonly Guid WinTrustActionGenericVerifyV2 = new("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");

    private nint _process;
    private nint _thread;
    private readonly string _sessionDirectory;
    private bool _disposed;

    private DecoyParent(nint process, nint thread, int pid, string stagedPath, string sessionDirectory)
    {
        _process = process;
        _thread = thread;
        Pid = pid;
        StagedPath = stagedPath;
        _sessionDirectory = sessionDirectory;
    }

    public int Pid { get; }

    /// <summary>The Games.exe in the session folder: &lt;game folder&gt;\_Hi3_Hg\parent\&lt;this pid&gt;\&lt;guid&gt;\Games.exe (the plugin's layout).</summary>
    public string StagedPath { get; }

    /// <summary>Where the decoy's image comes from: the configured path, else PlatformProcess.exe in the game's folder; a relative path is relative to that folder.</summary>
    public static string ResolveSource(string gameExecutable, string? configuredSource)
    {
        var gameDirectory = Path.GetDirectoryName(Path.GetFullPath(gameExecutable))!;
        return Path.GetFullPath(string.IsNullOrWhiteSpace(configuredSource) ? DefaultSource : configuredSource, gameDirectory);
    }

    /// <summary>
    /// Checks the source, requires valid Authenticode signatures on it and on the game with one signer certificate (unless <paramref name="verifySigner"/> is
    /// off, tests only), copies it to a fresh session folder as Games.exe, verifies the copy again and starts it suspended.
    /// </summary>
    public static DecoyParent Create(string gameExecutable, string? configuredSource, bool verifySigner = true)
    {
        var gameDirectory = Path.GetDirectoryName(Path.GetFullPath(gameExecutable))!;
        var source = ResolveSource(gameExecutable, configuredSource);
        if (!string.Equals(Path.GetExtension(source), ".exe", StringComparison.OrdinalIgnoreCase) || !File.Exists(source))
            throw new FileNotFoundException("the decoy parent's source EXE does not exist", source);
        if (verifySigner && !HasMatchingSigner(source, gameExecutable))
            throw new InvalidDataException("the decoy parent's source has no valid signature or a different signer than the game");

        SweepStale(gameDirectory);
        var directory = Path.Combine(gameDirectory, "_Hi3_Hg", "parent", Environment.ProcessId.ToString(), Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var staged = Path.Combine(directory, ImageName);
            File.Copy(source, staged);
            File.SetAttributes(staged, FileAttributes.Normal);
            if (verifySigner && !HasMatchingSigner(staged, gameExecutable))
                throw new InvalidDataException("the staged decoy parent fails signature verification");
            var (process, thread, pid) = Suspend(staged);
            return new DecoyParent(process, thread, pid, staged, directory);
        }
        catch
        {
            RemoveSessionDirectory(directory);
            throw;
        }
    }

    /// <summary>
    /// CreateProcessW(game, "game" arguments) with the decoy as the parent (STARTUPINFOEX + the one attribute, EXTENDED_STARTUPINFO_PRESENT, no handle
    /// inheritance), then a check that the OS reports the decoy as the new process' parent.
    /// </summary>
    public Process StartGame(string gameExecutable, string? arguments, string? workingDirectory)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        nint size = 0;
        InitializeProcThreadAttributeList(0, 1, 0, ref size); // the sizing call fails by design
        var attributeList = Marshal.AllocHGlobal(size);
        nint commandLinePtr = 0;
        nint parentSlot = 0;
        var initialized = false;
        try
        {
            if (!InitializeProcThreadAttributeList(attributeList, 1, 0, ref size))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "InitializeProcThreadAttributeList failed");
            initialized = true;
            // lpValue is a pointer to the parent's process handle
            parentSlot = Marshal.AllocHGlobal(nint.Size);
            Marshal.WriteIntPtr(parentSlot, _process);
            if (!UpdateProcThreadAttribute(attributeList, 0, ProcThreadAttributeParentProcess, parentSlot, nint.Size, 0, 0))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "UpdateProcThreadAttribute failed");

            if (!string.IsNullOrEmpty(arguments)) commandLinePtr = Marshal.StringToCoTaskMemUni($"\"{gameExecutable}\" {arguments}");
            var startup = new StartupInfoEx { Info = new StartupInfo { cb = Marshal.SizeOf<StartupInfoEx>() }, AttributeList = attributeList };
            if (!CreateProcessW(gameExecutable, commandLinePtr, 0, 0, false, ExtendedStartupInfoPresent, 0, workingDirectory, ref startup, out var info))
            {
                var error = Marshal.GetLastWin32Error();
                throw new Win32Exception(error, error == 740 ? "Endfield.exe needs administrator rights and this launcher is not elevated" : "CreateProcessW failed");
            }

            CloseHandle(info.hThread);
            try
            {
                var seen = ParentPidOf(info.hProcess);
                if (seen != Pid)
                {
                    TerminateProcess(info.hProcess, 1);
                    throw new InvalidOperationException($"the game's parent is pid {seen}, not the decoy (pid {Pid})");
                }

                return Process.GetProcessById(info.dwProcessId);
            }
            finally { CloseHandle(info.hProcess); }
        }
        finally
        {
            if (commandLinePtr != 0) Marshal.FreeCoTaskMem(commandLinePtr);
            if (parentSlot != 0) Marshal.FreeHGlobal(parentSlot);
            if (initialized) DeleteProcThreadAttributeList(attributeList);
            Marshal.FreeHGlobal(attributeList);
        }
    }

    /// <summary>Ends the decoy, waits for its teardown (2 s: the image file cannot be removed before) and removes the session folder.</summary>
    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_process != 0)
        {
            TerminateProcess(_process, 0);
            WaitForSingleObject(_process, 2000);
            CloseHandle(_process);
            _process = 0;
        }
        if (_thread != 0)
        {
            CloseHandle(_thread);
            _thread = 0;
        }
        RemoveSessionDirectory(_sessionDirectory);
    }

    /// <summary>Removes the session folders of owners that no longer run (a crashed launcher); one still in use stays.</summary>
    public static void SweepStale(string gameDirectory)
    {
        var root = Path.Combine(gameDirectory, "_Hi3_Hg", "parent");
        try
        {
            if (!Directory.Exists(root)) return;
            foreach (var owner in Directory.EnumerateDirectories(root))
            {
                if (!int.TryParse(Path.GetFileName(owner), out var pid) || pid <= 0 || pid == Environment.ProcessId) continue;
                try
                {
                    using var alive = Process.GetProcessById(pid);
                    if (!alive.HasExited) continue;
                }
                catch (ArgumentException) { }
                catch (InvalidOperationException) { }
                RemoveSessionDirectory(owner);
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }

    private static void RemoveSessionDirectory(string directory)
    {
        try
        {
            if (Directory.Exists(directory)) Directory.Delete(directory, true);
            var owner = Path.GetDirectoryName(directory);
            if (owner is not null && Directory.Exists(owner) && !Directory.EnumerateFileSystemEntries(owner).Any()) Directory.Delete(owner);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }

    private static (nint Process, nint Thread, int Pid) Suspend(string executablePath)
    {
        var startup = new StartupInfoEx { Info = new StartupInfo { cb = Marshal.SizeOf<StartupInfo>() } };
        if (!CreateProcessW(executablePath, 0, 0, 0, false, CreateSuspended | CreateNoWindow, 0, Path.GetDirectoryName(executablePath), ref startup, out var info))
        {
            var error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, $"cannot create the suspended decoy parent from {executablePath}");
        }
        return (info.hProcess, info.hThread, info.dwProcessId);
    }

    private static int ParentPidOf(nint process)
    {
        var basic = new ProcessBasicInformation();
        return NtQueryInformationProcess(process, 0, ref basic, Marshal.SizeOf<ProcessBasicInformation>(), out _) == 0 ? (int)basic.InheritedFromUniqueProcessId : 0;
    }

    /// <summary>Valid Authenticode signatures on both images and the same signer certificate (SHA-256 of the certificate), no publisher-name list.</summary>
    internal static bool HasMatchingSigner(string executablePath, string gameExecutablePath)
    {
        if (!File.Exists(executablePath)) return false;
        if (WinVerifyTrustFile(gameExecutablePath) != 0) return false;
        if (WinVerifyTrustFile(executablePath) != 0) return false;
        try
        {
            using var certificate = X509Certificate.CreateFromSignedFile(executablePath);
            using var gameCertificate = X509Certificate.CreateFromSignedFile(gameExecutablePath);
            return CryptographicOperations.FixedTimeEquals(SHA256.HashData(certificate.GetRawCertData()), SHA256.HashData(gameCertificate.GetRawCertData()));
        }
        catch (Exception ex) when (ex is CryptographicException or IOException) { return false; }
    }

    private static uint WinVerifyTrustFile(string filePath)
    {
        var nativeFilePath = Marshal.StringToCoTaskMemUni(filePath);
        var nativeFileInfo = Marshal.AllocHGlobal(Marshal.SizeOf<WintrustFileInfo>());
        var nativeTrustData = Marshal.AllocHGlobal(Marshal.SizeOf<WintrustData>());
        try
        {
            var fileInfo = new WintrustFileInfo { cbStruct = (uint)Marshal.SizeOf<WintrustFileInfo>(), pcwszFilePath = nativeFilePath };
            Marshal.StructureToPtr(fileInfo, nativeFileInfo, false);
            var trustData = new WintrustData
            {
                cbStruct = (uint)Marshal.SizeOf<WintrustData>(),
                dwUIChoice = 2,            // WTD_UI_NONE
                fdwRevocationChecks = 0,   // WTD_REVOKE_NONE
                dwUnionChoice = 1,         // WTD_CHOICE_FILE
                pFile = nativeFileInfo,
                dwStateAction = 1,         // WTD_STATEACTION_VERIFY
                dwProvFlags = 0x100,       // WTD_CACHE_ONLY_URL_RETRIEVAL
            };
            Marshal.StructureToPtr(trustData, nativeTrustData, false);
            var action = WinTrustActionGenericVerifyV2;
            var result = WinVerifyTrust(0, ref action, nativeTrustData);
            trustData.dwStateAction = 2;   // WTD_STATEACTION_CLOSE
            Marshal.StructureToPtr(trustData, nativeTrustData, false);
            WinVerifyTrust(0, ref action, nativeTrustData);
            return result;
        }
        finally
        {
            Marshal.FreeHGlobal(nativeTrustData);
            Marshal.FreeHGlobal(nativeFileInfo);
            Marshal.FreeCoTaskMem(nativeFilePath);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfo
    {
        public int cb;
        public nint lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public nint lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfoEx
    {
        public StartupInfo Info;
        public nint AttributeList;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public nint hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessBasicInformation
    {
        public nint ExitStatus, PebBaseAddress, AffinityMask, BasePriority, UniqueProcessId, InheritedFromUniqueProcessId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WintrustFileInfo
    {
        public uint cbStruct;
        public nint pcwszFilePath, hFile, pgKnownSubject;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WintrustData
    {
        public uint cbStruct;
        public nint pPolicyCallbackData, pSIPClientData;
        public uint dwUIChoice, fdwRevocationChecks, dwUnionChoice;
        public nint pFile;
        public uint dwStateAction;
        public nint hWVTStateData, pwszURLReference;
        public uint dwProvFlags, dwUIContext;
        public nint pSignatureSettings;
    }

    [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcessW(string? applicationName, nint commandLine, nint processAttributes, nint threadAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandles, uint creationFlags, nint environment, string? currentDirectory,
        ref StartupInfoEx startupInfo, out ProcessInformation processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool InitializeProcThreadAttributeList(nint attributeList, int attributeCount, int flags, ref nint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateProcThreadAttribute(nint attributeList, uint flags, nint attribute, nint value, nint size, nint previousValue, nint returnSize);

    [DllImport("kernel32.dll")]
    private static extern void DeleteProcThreadAttributeList(nint attributeList);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcess(nint process, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(nint handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(nint handle);

    [DllImport("ntdll.dll")]
    private static extern int NtQueryInformationProcess(nint process, int informationClass, ref ProcessBasicInformation information, int length, out int returned);

    [DllImport("wintrust.dll", EntryPoint = "WinVerifyTrust")]
    private static extern uint WinVerifyTrust(nint hwnd, ref Guid action, nint data);
}
