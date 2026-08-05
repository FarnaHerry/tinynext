# make-dist.ps1 — build the release app and package it into a distributable zip.
#
# Windows notes:
#   * `mcpp pack` is not yet supported on Windows (mcpp error), so we assemble
#     the bundle manually: release exe + assets/ + engines/.
#   * The "current profile" build dir is tracked in target/.build_cache (line 2),
#     and `mcpp build --release` points it at the release output.
#   * The zip name/version is read from mcpp.toml so it never drifts from the
#     package version.
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

# aria2-next 引擎二进制（exeDir/engines/aria2-next.exe 运行时解析）。没有它，
# 发行包只有内置 tinyhttps 引擎可用。文件已 gitignore，机器上可能没下载，
# 所以存在就带上、缺失则警告（不中断打包）。
$enginesDir = Join-Path $root "engines"
if (Test-Path $enginesDir) {
    Copy-Item $enginesDir (Join-Path $dist "engines") -Recurse
    $engineFiles = (Get-ChildItem $enginesDir | ForEach-Object Name) -join ", "
    Write-Host "  engines: $engineFiles"
} else {
    Write-Warning "  engines/ 不存在 —— aria2-next 引擎二进制未打包（发行包仅 tinyhttps 引擎）"
}

Write-Host "== 4/4 compress =="
$zip = Join-Path $root "tinynext-v$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip -CompressionLevel Optimal
Write-Host "  produced: $zip"

$size = (Get-Item $zip).Length / 1MB
Write-Host ("done — {0} ({1:N1} MB)" -f $zip, $size)
