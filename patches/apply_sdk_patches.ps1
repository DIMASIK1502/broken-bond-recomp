param(
    [string]$SdkDir = "thirdparty\rexglue-sdk"
)

$projectRoot = Split-Path -Parent $PSScriptRoot
$sdkPath = Join-Path $projectRoot $SdkDir

# Applied in this order; later patches assume the earlier ones.
# Optional ones were written against an earlier SDK revision than the v0.10.0
# tag and do not apply to it yet: they are reported and skipped.
$patches = @(
    @{ File = "guest-counters-and-file-open-hook.patch"; Optional = $false },
    @{ File = "rexglue-sdk-v0.10.0.patch"; Optional = $false },
    # Android platform layer (surface, fibers, robust mutexes, libc++ chrono).
    # Every change is behind ANDROID / REX_PLATFORM_ANDROID.
    @{ File = "android-platform.patch"; Optional = $false }
)

if (-not (Test-Path $sdkPath)) {
    Write-Error "SDK directory not found: $sdkPath"
    Write-Error "Run setup.ps1 first to clone the SDK."
    exit 1
}

Write-Host "Applying SDK patches to $sdkPath ..."
Push-Location $sdkPath
$failed = $false
try {
    foreach ($patch in $patches) {
        $patchFile = Join-Path $PSScriptRoot "sdk\$($patch.File)"
        if (-not (Test-Path $patchFile)) {
            Write-Error "Patch file not found: $patchFile"
            $failed = $true
            continue
        }
        git apply --reverse --check $patchFile 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  already applied: $($patch.File)"
            continue
        }
        $applyResult = git apply $patchFile 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  applied: $($patch.File)"
        } elseif ($patch.Optional) {
            Write-Warning "  skipped (does not apply to this SDK revision): $($patch.File)"
        } else {
            Write-Error "  failed: $($patch.File): $applyResult"
            $failed = $true
        }
    }
}
finally {
    Pop-Location
}
if ($failed) { exit 1 }
Write-Host "SDK patches done."
# A skipped optional patch leaves git's exit code behind; setup.ps1 reads it.
exit 0
