# Migrate local image files to covers directory
# Usage: migrate-images.ps1 <config_dir>

param(
    [Parameter(Mandatory=$true)]
    [string]$ConfigDir
)

$appsJsonPath = Join-Path $ConfigDir "apps.json"
$coversDir = Join-Path $ConfigDir "covers"

# Check if apps.json exists
if (-not (Test-Path $appsJsonPath)) {
    Write-Host "apps.json not found at: $appsJsonPath"
    exit 0
}

Write-Host "Reading apps.json from: $appsJsonPath"

# Read and parse apps.json
try {
    $appsJson = Get-Content $appsJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
} catch {
    Write-Host "Error parsing apps.json: $_"
    exit 1
}

# Ensure covers directory exists
if (-not (Test-Path $coversDir)) {
    New-Item -ItemType Directory -Path $coversDir -Force | Out-Null
    Write-Host "Created covers directory: $coversDir"
}

$modified = $false
$migratedCount = 0

# Process each app
if ($appsJson.apps) {
    foreach ($app in $appsJson.apps) {
        if (-not $app.'image-path') {
            continue
        }

        $imagePath = $app.'image-path'
        
        # Skip if already using desktop or boxart path
        if ($imagePath -eq 'desktop' -or $imagePath -match '^[^/\\]+\.png$') {
            Write-Host "Skipping already migrated: $($app.name) -> $imagePath"
            continue
        }

        # Check if it's an absolute path or relative path pointing to a real file
        $sourceFile = $null
        
        # Try as absolute path
        if (Test-Path $imagePath) {
            $sourceFile = $imagePath
        }
        # Try relative to config dir
        elseif (Test-Path (Join-Path $ConfigDir $imagePath)) {
            $sourceFile = Join-Path $ConfigDir $imagePath
        }
        # Try relative to old Sunshine root (parent of config)
        elseif (Test-Path (Join-Path (Split-Path $ConfigDir -Parent) $imagePath)) {
            $sourceFile = Join-Path (Split-Path $ConfigDir -Parent) $imagePath
        }

        if ($sourceFile -and (Test-Path $sourceFile)) {
            Write-Host "Found image file: $sourceFile"
            
            # Generate new filename with hash to avoid name conflicts
            # Use MD5 hash of the original filename to ensure uniqueness
            $originalFileName = [System.IO.Path]::GetFileNameWithoutExtension($sourceFile)
            $md5 = [System.Security.Cryptography.MD5]::Create()
            $hashBytes = $md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($originalFileName))
            $hash = [System.BitConverter]::ToString($hashBytes).Replace("-", "").Substring(0, 8).ToLower()
            
            $timestamp = [DateTimeOffset]::Now.ToUnixTimeSeconds()
            $extension = [System.IO.Path]::GetExtension($sourceFile)
            if (-not $extension) {
                $extension = ".png"
            }
            
            # Use hash instead of sanitized name to preserve uniqueness
            $newFileName = "app_${hash}_${timestamp}${extension}"
            $destinationPath = Join-Path $coversDir $newFileName
            
            # Copy file to covers directory
            try {
                Copy-Item -Path $sourceFile -Destination $destinationPath -Force
                Write-Host "✅ Migrated: $($app.name)"
                Write-Host "   From: $sourceFile"
                Write-Host "   To:   $destinationPath"
                Write-Host "   New path: $newFileName"
                
                # Update apps.json with new path (just the filename, no directory)
                $app.'image-path' = $newFileName
                $modified = $true
                $migratedCount++
            } catch {
                Write-Host "❌ Failed to copy image for $($app.name): $_"
            }
        } else {
            Write-Host "⚠️  Image file not found for $($app.name): $imagePath"
            # Keep the original path in case it's a network path or will be available later
        }
    }
}

# Save updated apps.json if modified
if ($modified) {
    try {
        $jsonOutput = $appsJson | ConvertTo-Json -Depth 10 -Compress:$false
        $jsonOutput | Set-Content -Path $appsJsonPath -Encoding UTF8 -NoNewline
        Write-Host ""
        Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        Write-Host "✅ Successfully migrated $migratedCount image(s)"
        Write-Host "   Updated: $appsJsonPath"
        Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    } catch {
        Write-Host "❌ Error saving apps.json: $_"
        exit 1
    }
} else {
    Write-Host "No images to migrate."
}

exit 0
