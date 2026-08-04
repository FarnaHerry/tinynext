# make-dist.ps1 — build the release app and package it into a distributable zip.
#
# Windows notes:
#   * `mcpp pack` is not yet supported on Windows (mcpp error), so we assemble
#     the bundle manually: release exe + assets/ (fonts are resolved at runtime
#     from exeDir/assets/).
#   * The "current profile" build dir is tracked in target/.build_cache (line 2),
#     and `mcpp build --release` points it at the release output.
#
# Usage (from PowerShell in this directory):
#   .\make-dist.ps1

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$exeName = "tinynext.exe"
$version = "0.1.0"

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

Write-Host "== 4/4 compress =="
$zip = Join-Path $root "tinynext-v$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip -CompressionLevel Optimal
Write-Host "  produced: $zip"

$size = (Get-Item $zip).Length / 1MB
Write-Host ("done — {0} ({1:N1} MB)" -f $zip, $size)
