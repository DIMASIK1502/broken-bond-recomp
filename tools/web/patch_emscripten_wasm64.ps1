# wasm64 hands Emscripten JS i64 pointers as BigInt. Three spots in a stock
# Emscripten 6 / emdawnwebgpu tree then mix that BigInt with a Number and the
# page throws before the first WebGPU present. This rewrites the local emsdk
# copies. A tree that already has the edits is left alone.
$ErrorActionPreference = "Stop"
$emsdk = $env:EMSDK
if (-not $emsdk) {
    Write-Error "EMSDK is not set. Run emsdk_env before the browser build."
    exit 1
}

function Update-TextFile([string]$Path, [scriptblock]$Edit) {
    if (-not (Test-Path $Path)) {
        Write-Error "Missing $Path. Link once so Emscripten fetches emdawnwebgpu, then build again."
        exit 1
    }
    $text = [System.IO.File]::ReadAllText($Path)
    $next = & $Edit $text
    if ($null -eq $next) {
        Write-Host "already patched $Path"
        return
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($Path, $next, $utf8)
    Write-Host "patched $Path"
}

$emscripten = Join-Path $emsdk "upstream\emscripten"

Update-TextFile (Join-Path $emscripten "src\lib\libint53.js") {
    param($text)
    $old = @'
  $bigintToI53Checked: (num) => (num < INT53_MIN || num > INT53_MAX) ? NaN : Number(num),
'@
    $new = @'
  // Wasm64 passes i64 arguments as BigInt. Comparing a BigInt with a Number
  // throws, so the range check has to stay in BigInt until the value is known
  // to fit in a JS number. Pointers in this heap are inside that range.
  $bigintToI53Checked: (num) => {
    if (typeof num == 'bigint') {
      if (num < BigInt(INT53_MIN) || num > BigInt(INT53_MAX)) return NaN;
      return Number(num);
    }
    return (num < INT53_MIN || num > INT53_MAX) ? NaN : Number(num);
  },
'@
    if ($text.Contains($new.TrimEnd())) { return $null }
    if (-not $text.Contains($old.TrimEnd())) {
        throw "libint53.js has neither the stock bigintToI53Checked nor the wasm64 one"
    }
    return $text.Replace($old.TrimEnd(), $new.TrimEnd())
}

Update-TextFile (Join-Path $emscripten "src\lib\libstrings.js") {
    param($text)
    $oldFind = @'
  $findStringEnd: (heapOrArray, idx, maxBytesToRead, ignoreNul) => {
    var maxIdx = idx + maxBytesToRead;
'@
    $newFind = @'
  $findStringEnd: (heapOrArray, idx, maxBytesToRead, ignoreNul) => {
    if (typeof idx == 'bigint') idx = Number(idx);
    var maxIdx = idx + maxBytesToRead;
'@
    $oldUtf = @'
  $UTF8ToString: (ptr, maxBytesToRead, ignoreNul) => {
#if ASSERTIONS
'@
    $newUtf = @'
  $UTF8ToString: (ptr, maxBytesToRead, ignoreNul) => {
    if (typeof ptr == 'bigint') ptr = Number(ptr);
#if ASSERTIONS
'@
    $changed = $false
    if ($text.Contains($oldFind.TrimEnd())) {
        $text = $text.Replace($oldFind.TrimEnd(), $newFind.TrimEnd())
        $changed = $true
    } elseif (-not $text.Contains("if (typeof idx == 'bigint') idx = Number(idx);")) {
        throw "libstrings.js findStringEnd is neither stock nor wasm64"
    }
    if ($text.Contains($oldUtf.TrimEnd())) {
        $text = $text.Replace($oldUtf.TrimEnd(), $newUtf.TrimEnd())
        $changed = $true
    } elseif (-not $text.Contains("if (typeof ptr == 'bigint') ptr = Number(ptr);")) {
        throw "libstrings.js UTF8ToString is neither stock nor wasm64"
    }
    if (-not $changed) { return $null }
    return $text
}

$webgpu = Join-Path $emscripten "cache\ports\emdawnwebgpu\emdawnwebgpu_pkg\webgpu\src\library_webgpu.js"
Update-TextFile $webgpu {
    param($text)
    $pairs = @(
        @{
            Old = @'
        importJs${object}: (obj, parentPtr = 0) => {
          var ptr = _emwgpuCreate${object}(parentPtr);
          WebGPU.Internals.jsObjects[ptr] = obj;
'@
            New = @'
        importJs${object}: (obj, parentPtr = ${this.NULLPTR}) => {
          var ptr = _emwgpuCreate${object}(parentPtr);
          WebGPU.Internals.jsObjectInsert(ptr, obj);
'@
        },
        @{
            Old = 'importJsBuffer: (buffer, parentPtr = 0) => {'
            New = 'importJsBuffer: (buffer, parentPtr = {{{ gpu.NULLPTR }}}) => {'
        },
        @{
            Old = 'importJsDevice: (device, parentPtr = 0) => {'
            New = 'importJsDevice: (device, parentPtr = {{{ gpu.NULLPTR }}}) => {'
        },
        @{
            Old = @'
      jsObjects: [],
      jsObjectInsert: (ptr, jsObject) => {
        {{{ gpu.ensurePointerUnsigned('ptr') }}}
        WebGPU.Internals.jsObjects[ptr] = jsObject;
      },
'@
            New = @'
      jsObjects: Object.create(null),
      jsObjectInsert: (ptr, jsObject) => {
        if (typeof ptr === 'bigint') ptr = Number(ptr);
        {{{ gpu.ensurePointerUnsigned('ptr') }}}
        WebGPU.Internals.jsObjects[ptr] = jsObject;
      },
'@
        },
        @{
            Old = @'
    getJsObject: (ptr) => {
      if (!ptr) return undefined;
      {{{ gpu.ensurePointerUnsigned('ptr') }}}
'@
            New = @'
    getJsObject: (ptr) => {
      if (!ptr) return undefined;
      if (typeof ptr === 'bigint') ptr = Number(ptr);
      {{{ gpu.ensurePointerUnsigned('ptr') }}}
'@
        },
        @{
            Old = @'
  emwgpuDelete: (ptr) => {
    delete WebGPU.Internals.jsObjects[ptr];
  },
'@
            New = @'
  emwgpuDelete: (ptr) => {
    if (typeof ptr === 'bigint') ptr = Number(ptr);
    delete WebGPU.Internals.jsObjects[ptr];
  },
'@
        }
    )
    $changed = $false
    foreach ($pair in $pairs) {
        $old = $pair.Old.TrimEnd()
        $new = $pair.New.TrimEnd()
        if ($text.Contains($old)) {
            $text = $text.Replace($old, $new)
            $changed = $true
        } elseif (-not $text.Contains($new)) {
            throw "library_webgpu.js is missing both the stock and wasm64 form of: $($old.Substring(0, [Math]::Min(60, $old.Length)))"
        }
    }
    if (-not $changed) { return $null }
    return $text
}
