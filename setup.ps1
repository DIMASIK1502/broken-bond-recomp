# setup.ps1 - Bootstrap the Broken Bond ReXGlue port project.
#
# Clones the ReXGlue SDK into thirdparty/rexglue-sdk at the pinned version and
# initializes its nested submodules. Safe to re-run.

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdkDir = Join-Path $root "thirdparty\rexglue-sdk"
$tag = "v0.10.0"

Write-Host "== Naruto: The Broken Bond - ReXGlue project setup ==" -ForegroundColor Cyan

foreach ($tool in @("git", "cmake", "ninja", "clang")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Host "MISSING: $tool not found on PATH." -ForegroundColor Red
        Write-Host " ReXGlue requires: Clang 18+, CMake 3.25+, Ninja, Visual Studio 2022 (Windows SDK)."
        exit 1
    }
}
Write-Host "Prerequisites OK." -ForegroundColor Green

if (Test-Path (Join-Path $sdkDir ".git")) {
    Write-Host "SDK already cloned at $sdkDir"
} else {
    Write-Host "Cloning ReXGlue SDK ($tag) into thirdparty\rexglue-sdk ..."
    git clone --branch $tag --depth 1 https://github.com/rexglue/rexglue-sdk.git $sdkDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}

Write-Host "Initializing SDK submodules (this can take a while) ..."
git -C $sdkDir submodule update --init --recursive --depth 1
if ($LASTEXITCODE -ne 0) { throw "submodule init failed" }

# Without symlink support git writes libmspack's links as one-line text files,
# and clang then fails on "../../libmspack/mspack/lzxd.c". Copy the targets in.
$mspackDir = Join-Path $sdkDir "thirdparty\libmspack"
$links = git -C $mspackDir ls-files -s | Where-Object { $_ -match '^120000 ' } |
    ForEach-Object { ($_ -split "`t", 2)[1] }
foreach ($link in $links) {
    $linkPath = Join-Path $mspackDir $link
    if ((Get-Item $linkPath).Length -gt 256) { continue }
    $target = Join-Path (Split-Path $linkPath) ((Get-Content $linkPath -Raw).Trim())
    if (Test-Path $target -PathType Leaf) { Copy-Item $target $linkPath -Force }
}

# Host renderer dependencies (MIT, and BSD-2 for libadrenotools). Pinned by
# commit; none has release tags.
$hostDeps = @(
    @{ Name = "plume"; Url = "https://github.com/renderbag/plume.git";
       Commit = "0ce184b19270e49236afbb74052a6b267494b86b" },
    @{ Name = "xenosrecomp"; Url = "https://github.com/hedge-dev/XenosRecomp.git";
       Commit = "990d03b28a27b50277ee5d8d942e1c5f873869d1" },
    # Android arm64 only (BSD-2-Clause): loads a custom Vulkan driver such as
    # Turnip into the app on Snapdragon phones.
    @{ Name = "libadrenotools"; Url = "https://github.com/bylaws/libadrenotools.git";
       Commit = "8fae8ce254dfc1344527e05301e43f37dea2df80" }
)
foreach ($dep in $hostDeps) {
    $depDir = Join-Path $root "thirdparty\$($dep.Name)"
    if (-not (Test-Path (Join-Path $depDir ".git"))) {
        Write-Host "Cloning $($dep.Name) into thirdparty\$($dep.Name) ..."
        git clone $dep.Url $depDir
        if ($LASTEXITCODE -ne 0) { throw "git clone of $($dep.Name) failed" }
    }
    git -C $depDir checkout --quiet $dep.Commit
    if ($LASTEXITCODE -ne 0) { throw "checkout of $($dep.Name) at $($dep.Commit) failed" }
    git -C $depDir submodule update --init --recursive --depth 1
    if ($LASTEXITCODE -ne 0) { throw "submodule init of $($dep.Name) failed" }
}

# WebGPU backend of Plume. A separate tree: thirdparty/plume stays the native
# Vulkan/D3D12 renderer. The browser build is the only one that links this.
$plumeWebDir = Join-Path $root "thirdparty\plume-web"
$plumeWebCommit = "73a662579698c8eb954e0697220a8817cdc64472"
if (-not (Test-Path (Join-Path $plumeWebDir ".git"))) {
    Write-Host "Cloning plume-web into thirdparty\plume-web ..."
    git clone https://github.com/n64hub/plume-web.git $plumeWebDir
    if ($LASTEXITCODE -ne 0) { throw "git clone of plume-web failed" }
}
git -C $plumeWebDir checkout --quiet $plumeWebCommit
if ($LASTEXITCODE -ne 0) { throw "checkout of plume-web at $plumeWebCommit failed" }
git -C $plumeWebDir submodule update --init --recursive --depth 1
if ($LASTEXITCODE -ne 0) { throw "submodule init of plume-web failed" }
$plumeWebPatch = Join-Path $root "patches\plume-web\wasm64-watch-device.patch"
git -C $plumeWebDir apply --reverse --check $plumeWebPatch 2>&1 | Out-Null
if ($LASTEXITCODE -eq 0) {
    Write-Host "plume-web wasm64 patch already applied"
} else {
    Write-Host "Applying patches\plume-web\wasm64-watch-device.patch ..."
    git -C $plumeWebDir apply $plumeWebPatch
    if ($LASTEXITCODE -ne 0) { throw "plume-web wasm64 patch failed" }
}

# Broken Bond changes to XenosRecomp: fetch-slot descriptor indices, 1D
# textures and this game's vertex input locations.
$xenosRecompDir = Join-Path $root "thirdparty\xenosrecomp"
$xenosRecompPatch = Join-Path $root "patches\xenosrecomp\broken-bond.patch"
git -C $xenosRecompDir apply --reverse --check --ignore-whitespace $xenosRecompPatch 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Applying patches\xenosrecomp\broken-bond.patch ..."
    git -C $xenosRecompDir apply --ignore-whitespace $xenosRecompPatch
    if ($LASTEXITCODE -ne 0) { throw "XenosRecomp patch failed" }
}

# Broken Bond changes to Plume: Vulkan 1.2 core features without extension
# names (the Android emulator lists none of them).
$plumeDir = Join-Path $root "thirdparty\plume"
$plumePatch = Join-Path $root "patches\plume\broken-bond.patch"
git -C $plumeDir apply --reverse --check --ignore-whitespace $plumePatch 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Applying patches\plume\broken-bond.patch ..."
    git -C $plumeDir apply --ignore-whitespace $plumePatch
    if ($LASTEXITCODE -ne 0) { throw "Plume patch failed" }
}

$applySdkPatches = Join-Path $root "patches\apply_sdk_patches.ps1"
if (Test-Path $applySdkPatches) {
    Write-Host "Applying local SDK patches ..."
    & $applySdkPatches
    if ($LASTEXITCODE -ne 0) { throw "SDK patch application failed" }
}

Write-Host ""
Write-Host "Setup complete." -ForegroundColor Green
Write-Host "Next steps:"
Write-Host " 1. Extract the Xbox 360 disc into .\game\ (entrypoint at game\default.xex)"
Write-Host " 2. Build the SDK CLI: cmake --preset win-amd64-release -DREXSDK_DIR=thirdparty\rexglue-sdk ; cmake --build out\build\win-amd64-release --target rexglue"
Write-Host " 3. Configure & build the port: cmake --preset win-amd64-relwithdebinfo -DREXSDK_DIR=thirdparty\rexglue-sdk ; cmake --build out\build\win-amd64-relwithdebinfo"
Write-Host " 4. Run broken_bond.exe with --game_data_root=game --gpu_plugin=xenos --mnk_mode --render_target_path_d3d12=rov"
