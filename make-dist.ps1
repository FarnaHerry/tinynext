# make-dist.ps1 - build the release app and package it into a distributable zip.
#
# Windows notes:
#   * `mcpp pack` is not yet supported on Windows (mcpp error), so we assemble
#     the bundle manually: release exe + assets/ + engines/.
#   * The "current profile" build dir is tracked in target/.build_cache (line 2),
#     and `mcpp build --release` points it at the release output.
#   * The zip name/version is read from mcpp.toml so it never drifts from the
#     package version.
#
# IMPORTANT: keep this file ASCII-only. Windows PowerShell 5.1 reads scripts
# without a UTF-8 BOM as ANSI; a non-ASCII byte (e.g. the em-dash U+2014, whose
# UTF-8 last byte 0x94 decodes to the smart-quote U+201D in ANSI) then becomes a
# quote character PowerShell treats as a string delimiter, breaking the parse.
#
# Usage (from PowerShell in this directory):
#   .\make-dist.ps1

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$exeName = "tinynext.exe"

# Version comes from mcpp.toml (line: version = "x.y.z"), not a hardcoded copy.
$versionLine = Get-Content (Join-Path $root "mcpp.toml") |
    Where-Object { $_ -match '^\s*version\s*=\s*"' } | Select-Object -First 1
if (-not $versionLine) { throw "cannot parse version from mcpp.toml" }
$version = $versionLine -replace '^\s*version\s*=\s*"([^"]+)".*', '$1'

# 重新生成版本头（src/versions.generated.h）：版本只在 mcpp.toml 维护，打包前
# 跑一次，确保 exe 里的版本字符串与资源 FILEVERSION 一致。
& "$root\scripts\gen-versions.ps1"

Write-Host "== 1/4 build release =="
Push-Location $root
try { mcpp build --release | Out-Host } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw "mcpp build --release failed" }

Write-Host "== 2/4 locate current build dir =="
$cacheLines = Get-Content (Join-Path $root "target/.build_cache")
$buildDir = $cacheLines[1].Trim()
$exe = Join-Path $buildDir "bin/$exeName"
if (-not (Test-Path $exe)) { throw "release exe not found at $exe" }
Write-Host "  exe: $exe"

Write-Host "== 3/4 stage dist\ =="
$dist = Join-Path $root "dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
Copy-Item $exe (Join-Path $dist $exeName)
Copy-Item (Join-Path $root "assets") (Join-Path $dist "assets") -Recurse

# aria2-next engine binary (resolved at runtime as exeDir/engines/aria2-next.exe).
# aria2-next is the ONLY download engine; without it downloads will not work. The
# engines dir is gitignored and may be absent on some machines, so copy if present,
# warn if missing (do not abort packaging).
$enginesDir = Join-Path $root "engines"
if (Test-Path $enginesDir) {
    Copy-Item $enginesDir (Join-Path $dist "engines") -Recurse
    $engineFiles = (Get-ChildItem $enginesDir | ForEach-Object Name) -join ", "
    Write-Host "  engines: $engineFiles"
} else {
    Write-Warning "  engines/ missing - aria2-next engine NOT packaged (downloads will not work)"
}

Write-Host "== 4/4 compress =="
$zip = Join-Path $root "tinynext-v$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip -CompressionLevel Optimal
Write-Host "  produced: $zip"

$size = (Get-Item $zip).Length / 1MB
Write-Host ("done - {0} ({1:N1} MB)" -f $zip, $size)
