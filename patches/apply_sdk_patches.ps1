param(
    [string]$SdkDir = "thirdparty\rexglue-sdk"
)

$projectRoot = Split-Path -Parent $PSScriptRoot
$sdkPath = Join-Path $projectRoot $SdkDir
$patchFile = Join-Path $PSScriptRoot "sdk\rexglue-sdk-v0.10.0.patch"

if (-not (Test-Path $sdkPath)) {
    Write-Error "SDK directory not found: $sdkPath"
    Write-Error "Run setup.ps1 first to clone the SDK."
    exit 1
}

if (-not (Test-Path $patchFile)) {
    Write-Error "Patch file not found: $patchFile"
    exit 1
}

Write-Host "Applying SDK patches to $sdkPath ..."
Push-Location $sdkPath
try {
    git apply --reverse --check $patchFile 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "SDK patches already applied."
        exit 0
    }

    git apply --check $patchFile 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Attempting to apply with --3way..."
        $applyResult = git apply --3way $patchFile 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to apply SDK patches: $applyResult"
            exit 1
        }
    } else {
        $applyResult = git apply $patchFile 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to apply SDK patches: $applyResult"
            exit 1
        }
    }
    Write-Host "SDK patches applied successfully."
}
finally {
    Pop-Location
}
