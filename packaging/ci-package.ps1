# ci-package.ps1 — GitHub Actions 专用 Windows 打包（编译只一次）。
#
# 与本地 make-dist.ps1 的区别：CI 的 build job 已编好 tinynext.exe 并作为
# artifact 传入，本脚本**不编译、不读 target/.build_cache**，只把已就位的
# exe + engines + assets 组装成 dist\ 与便携 zip。NSIS 安装包由 workflow 在
# dist\ 就绪后单独跑 makensis（against dist\，见 tinynext.nsi）。
#
# 用法：powershell -File packaging/ci-package.ps1 <exe路径>
#   exe路径：build job 上传的 tinynext.exe（已在 dist-src/tinynext.exe）。
#   engines/（aria2/yt-dlp/ffmpeg）与 assets/ 须已就位于仓库根。
#
# IMPORTANT: keep this file ASCII-only (same rule as make-dist.ps1).

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot   # repo root (ci-package.ps1 lives in packaging/)
$exe = $args[0]
if (-not $exe) { throw "usage: ci-package.ps1 <exe-path>" }

# 版本来自 mcpp.toml（不硬编码）。
$versionLine = Get-Content (Join-Path $root "mcpp.toml") |
    Where-Object { $_ -match '^\s*version\s*=\s*"' } | Select-Object -First 1
if (-not $versionLine) { throw "cannot parse version from mcpp.toml" }
$version = $versionLine -replace '^\s*version\s*=\s*"([^"]+)".*', '$1'

Write-Host "== stage dist\ =="
$dist = Join-Path $root "dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $dist "engines") | Out-Null
Copy-Item $exe (Join-Path $dist "tinynext.exe")
Copy-Item (Join-Path $root "assets") (Join-Path $dist "assets") -Recurse
if (Test-Path (Join-Path $root "THIRD-PARTY-NOTICES.md")) {
    Copy-Item (Join-Path $root "THIRD-PARTY-NOTICES.md") (Join-Path $dist "THIRD-PARTY-NOTICES.md")
}

# engines/: aria2/yt-dlp/ffmpeg 均已由 package job 就位（build+trim-ffmpeg）。
foreach ($name in @("aria2-next.exe", "yt-dlp.exe", "ffmpeg.exe")) {
    $src = Join-Path $root "engines/$name"
    if (Test-Path $src) { Copy-Item $src (Join-Path $dist "engines/$name") }
    else { Write-Warning "engines/$name missing — could not package" }
}
if (Test-Path (Join-Path $root "engines/checksums.sha256")) {
    Copy-Item (Join-Path $root "engines/checksums.sha256") (Join-Path $dist "engines/checksums.sha256")
}

Write-Host "== compress zip =="
$zip = Join-Path $root "tinynext-v$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip -CompressionLevel Optimal
$size = (Get-Item $zip).Length / 1MB
Write-Host "produced: $zip ($('{0:N1}' -f $size) MB)"
