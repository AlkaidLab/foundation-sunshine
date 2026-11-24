[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)] [string] $CorePath,
  [Parameter(Mandatory = $true)] [string] $CliPath,
  [ValidateSet('toml', 'loopback', 'deny')] [string] $Mode = 'toml'
)

# Isolated diagnostic only. A loopback listener is not local-user authentication.
# No TUN, peer or transport listener is configured; the test secret is disposable.
$ErrorActionPreference = 'Stop'
if (@(Get-ChildItem Env:ET_* -ErrorAction SilentlyContinue).Count -gt 0) {
  throw 'Unset inherited ET_* variables before running this diagnostic.'
}
foreach ($path in @($CorePath, $CliPath)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Executable not found: $path" }
}

$portProbe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$port = $portProbe.LocalEndpoint.Port
$portProbe.Stop()
$id = [guid]::NewGuid().ToString('N')
$tempParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempParent ("sunshine-easytier-rpc-probe-" + $id)
$coreProcess = $null
$clientProcess = $null
try {
  New-Item -Path $root -ItemType Directory | Out-Null
  $config = Join-Path $root 'probe.toml'
  @"
instance_name = "sunshine-rpc-probe"
ipv4 = "10.197.43.1/24"
dhcp = false
listeners = []
rpc_portal = "127.0.0.1:$port"
[network_identity]
network_name = "sunshine-rpc-probe"
network_secret = "disposable-probe-secret"
[flags]
no_tun = true
disable_upnp = true
"@ | Set-Content -LiteralPath $config -Encoding Ascii

  $argsList = @('--config-file', ('"' + $config + '"'), '--no-listener', '--no-tun', '--disable-upnp')
  if ($Mode -ne 'toml') { $argsList += @('--rpc-portal', "127.0.0.1:$port") }
  if ($Mode -eq 'deny') { $argsList += @('--rpc-portal-whitelist', '192.0.2.0/24') }
  $coreProcess = Start-Process -FilePath $CorePath -ArgumentList $argsList -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $root 'core.log') -RedirectStandardError (Join-Path $root 'core-error.log')
  $listener = $null
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    if ($coreProcess.HasExited) { throw "EasyTier exited before opening RPC (exit $($coreProcess.ExitCode))" }
    $listener = Get-NetTCPConnection -State Listen -OwningProcess $coreProcess.Id -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($listener) { break }
  }
  if (-not $listener) { throw 'No RPC listener observed for the probe process.' }

  $clientOutput = Join-Path $root 'client.log'
  $clientProcess = Start-Process -FilePath $CliPath -ArgumentList @('-p', "127.0.0.1:$($listener.LocalPort)", 'node', 'config') `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput $clientOutput -RedirectStandardError (Join-Path $root 'client-error.log')
  $completed = $clientProcess.WaitForExit(5000)
  $response = if ($completed) { Get-Content -LiteralPath $clientOutput -Raw -ErrorAction SilentlyContinue } else { '' }
  [pscustomobject]@{
    Mode = $Mode
    ConfiguredPort = $port
    ListenerAddress = $listener.LocalAddress
    ListenerPort = $listener.LocalPort
    ClientCompleted = $completed
    ClientExitCode = if ($completed) { $clientProcess.ExitCode } else { $null }
    TestSecretReadable = [bool]($response -and $response.Contains('disposable-probe-secret'))
  }
} finally {
  foreach ($process in @($clientProcess, $coreProcess)) {
    if ($process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
      $process.WaitForExit()
    }
  }
  if ([System.IO.Path]::GetFullPath($root) -eq (Join-Path $tempParent ("sunshine-easytier-rpc-probe-" + $id))) {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
  }
}
