# Read-only policy checks. Never repair permissions on a pre-existing directory.
function Get-MatTrustedHash($Manifest,[string]$Name){
 $entries=@($Manifest.binaries|Where-Object name -eq $Name)
 if($entries.Count -ne 1 -or $entries[0].sha256 -notmatch '^[0-9a-f]{64}$'){throw "Required runtime hash missing/invalid: $Name"}
 return [string]$entries[0].sha256
}
function Assert-MatBuildArtifacts([string]$Root,$Manifest){
 foreach($name in @('sunshine-plugin-mat-capture.exe','sunshine-plugin-object-tap.dll','sunshine-sonic-mat-worker.dll')){
  if((Get-FileHash -LiteralPath (Join-Path $Root $name)).Hash.ToLower() -ne (Get-MatTrustedHash $Manifest $name)){throw "Binary differs from build: $name"}
 }
}
# The build directory is user-writable, so its files may change after the
# manifest check. Trust is established only on the copy in the protected
# deployment directory, compared against the manifest hash.
function Copy-MatDeploymentFile([string]$From,[string]$To,[string]$Hash){
 $copied=$false
 if(!(Test-Path -LiteralPath $To)){Copy-Item -LiteralPath $From -Destination $To;$copied=$true}
 if((Get-FileHash -LiteralPath $To).Hash.ToLower() -ne $Hash){
  if($copied){Remove-Item -LiteralPath $To -Force}
  throw "Deployed binary differs from build manifest: $([IO.Path]::GetFileName($To))"
 }
}
function Assert-MatDeploymentAcl($Acl){
 $trusted=@('S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
 if($Acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin $trusted){throw 'Deployment owner is not trusted'}
 $write=[Security.AccessControl.FileSystemRights]'Write,Delete,DeleteSubdirectoriesAndFiles,ChangePermissions,TakeOwnership'
 foreach($rule in $Acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
  if($rule.AccessControlType -eq 'Allow' -and !($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) -and
     ($rule.FileSystemRights -band $write) -and $rule.IdentityReference.Value -notin $trusted){throw 'Deployment permits untrusted writes'}
 }
}
function Assert-MatDeploymentEntry([string]$Path){
 $item=Get-Item -LiteralPath $Path -Force
 if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Deployment reparse points are forbidden'}
 Assert-MatDeploymentAcl (Get-Acl -LiteralPath $Path)
}
function Initialize-MatDeploymentDirectory([string]$Path){
 if(!(Test-Path -LiteralPath $Path)){New-Item -ItemType Directory -Path $Path|Out-Null}
 if(!(Get-Item -LiteralPath $Path -Force).PSIsContainer){throw 'Deployment directory required'}
 Assert-MatDeploymentEntry $Path
}
