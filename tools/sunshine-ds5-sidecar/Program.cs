using System.Reflection;
using System.Security.Principal;
using System.Text.Json;
using HIDMaestro;
using Sunshine.Ds5Sidecar;

var pipeName = "sunshine-ds5-v1";
var probe = false;
var selfCheck = false;
string? selfTestProfile = null;
string? resultPath = null;
string? audioWriterPath = null;
for (var i = 0; i < args.Length; i++)
{
    if (args[i] == "--pipe" && i + 1 < args.Length)
        pipeName = args[++i];
    else if (args[i] == "--probe")
        probe = true;
    else if (args[i] == "--self-check")
        selfCheck = true;
    else if (args[i] == "--self-test" && i + 1 < args.Length)
        selfTestProfile = args[++i];
    else if (args[i] == "--result" && i + 1 < args.Length)
        resultPath = args[++i];
    else if (args[i] == "--audio-writer" && i + 1 < args.Length)
        audioWriterPath = args[++i];
    else
        return Fail($"Unknown argument: {args[i]}");
}

if (selfCheck)
{
    ProtocolSelfTest.RunDeterministicChecks();
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        audio_layout = true,
        channel_isolation = true,
        default_endpoint_classification = true,
        genshin_compatibility_identity = true,
        audio_policy_violation = true,
    }));
    return 0;
}

if (probe)
{
    ProtocolSelfTest.RunDeterministicChecks();
    using var context = new HMContext();
    context.LoadDefaultProfiles();
    var standard = context.GetProfile("dualsense");
    var composite = context.GetProfile("dualsense-composite");
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        protocol = Protocol.Version,
        runtime_version = typeof(HMContext).Assembly.GetCustomAttribute<AssemblyFileVersionAttribute>()?.Version,
        elevated = IsElevated(),
        standard = standard is not null,
        composite = composite is not null,
        genshin_compatibility_identity = true,
        audio_policy_violation = true,
        driver_installed = context.IsDriverInstalled,
        usbip_available = HMContext.IsUsbipBackendAvailable,
    }));
    return 0;
}

if (selfTestProfile is not null)
{
    if (!IsElevated())
        return Fail("sunshine-ds5-sidecar protocol self-test must run elevated");
    if (selfTestProfile is not ("standard" or "composite"))
        return Fail("--self-test must be 'standard' or 'composite'");
    try
    {
        return await ProtocolSelfTest.RunAsync(selfTestProfile == "composite", resultPath, audioWriterPath);
    }
    catch (Exception ex)
    {
        if (!string.IsNullOrWhiteSpace(resultPath))
            await File.WriteAllTextAsync(resultPath, JsonSerializer.Serialize(new { error = ex.ToString() }));
        return Fail(ex.ToString());
    }
}

if (!IsElevated())
    return Fail("sunshine-ds5-sidecar must run elevated to manage virtual devices");

using var shutdown = new CancellationTokenSource();
Console.CancelKeyPress += (_, eventArgs) =>
{
    eventArgs.Cancel = true;
    shutdown.Cancel();
};

await using var server = new SidecarServer(pipeName);
try
{
    await server.RunAsync(shutdown.Token);
    return 0;
}
catch (OperationCanceledException)
{
    return 0;
}
catch (Exception ex)
{
    return Fail(ex.ToString());
}

static bool IsElevated()
{
    using var identity = WindowsIdentity.GetCurrent();
    return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
}

static int Fail(string message)
{
    Console.Error.WriteLine(message);
    return 1;
}
