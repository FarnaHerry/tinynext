# make-win-pkg.ps1 - build the release + stage dist\ (via make-dist.ps1) and
# compile the NSIS installer (tinynext.nsi).
#
# Usage (from PowerShell):
#   .\packaging\make-win-pkg.ps1
#
# Requires NSIS (makensis) on PATH. CI installs it with: choco install nsis
#
# Outputs (repo root):
#   tinynext-v<ver>-win64.zip          (portable, from make-dist.ps1)
#   tinynext-v<ver>-win64-setup.exe    (NSIS installer)
#
# IMPORTANT: keep this file ASCII-only (same rule as make-dist.ps1).

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot   # repo root (make-win-pkg.ps1 lives in packaging/)

Write-Host "== 1/3 make-dist.ps1 (build + stage dist\ + zip) =="
Push-Location $repoRoot
try { & ".\make-dist.ps1" | Out-Host } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw "make-dist.ps1 failed" }

Write-Host "== 2/3 parse version =="
$versionLine = Get-Content (Join-Path $repoRoot "mcpp.toml") |
    Where-Object { $_ -match '^\s*version\s*=\s*"' } | Select-Object -First 1
if (-not $versionLine) { throw "cannot parse version from mcpp.toml" }
$version = $versionLine -replace '^\s*version\s*=\s*"([^"]+)".*', '$1'

Write-Host "== 3/3 makensis =="
$makensis = Get-Command makensis -ErrorAction SilentlyContinue
if (-not $makensis) { throw "makensis not found - install NSIS (choco install nsis)" }
& $makensis.Source "/DAPP_VERSION=$version" (Join-Path $PSScriptRoot "tinynext.nsi")
if ($LASTEXITCODE -ne 0) { throw "makensis failed" }

$out = Join-Path $repoRoot "tinynext-v$version-win64-setup.exe"
$size = (Get-Item $out).Length / 1MB
Write-Host ("done - {0} ({1:N1} MB)" -f $out, $size)
