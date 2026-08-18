#!/usr/bin/env bash
# ci-package.sh — GitHub Actions 专用打包脚本（编译只一次）。
#
# 与本地 make-dist.sh 的区别：CI 的 build job 已经编好 exe 并作为 artifact
# 传进来，本脚本**不编译、不遍历易错的 target/<hash>/ 多目录**，只把已就位的
# exe + engines + assets 组装成发行包。
#
# 用法：bash packaging/ci-package.sh <linux|macos> <x86_64|arm64> <exe路径>
#   exe路径：build job 上传的 tinynext 二进制（已在 dist-src/tinynext）。
#   engines/（aria2/yt-dlp/ffmpeg）与 assets/ 须已就位于仓库根。

set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
os="${1:?usage: ci-package.sh <linux|macos> <arch> <exe>}"
arch="${2:?usage: ci-package.sh <os> <arch> <exe>}"
exe="${3:?usage: ci-package.sh <os> <arch> <exe>}"
version="$(grep -m1 '^version' "$root/mcpp.toml" | sed -E 's/.*"([^"]+)".*/\1/')"

#
dist="$root/dist"
rm -rf "$dist"
mkdir -p "$dist/engines"

cp "$exe" "$dist/tinynext"
chmod +x "$dist/tinynext"
cp -r "$root/assets" "$dist/assets"
if [ -f "$root/THIRD-PARTY-NOTICES.md" ]; then
    cp "$root/THIRD-PARTY-NOTICES.md" "$dist/THIRD-PARTY-NOTICES.md"
fi

# engines/: aria2/yt-dlp/ffmpeg 均已由 package job 就位（build+trim-ffmpeg）。
for name in aria2-next yt-dlp ffmpeg; do
    if [ -x "$root/engines/$name" ]; then
        cp "$root/engines/$name" "$dist/engines/$name"
        chmod +x "$dist/engines/$name"
    else
        echo "WARN: engines/$name missing — could not package" >&2
    fi
done
if [ -f "$root/engines/checksums.sha256" ]; then
    cp "$root/engines/checksums.sha256" "$dist/engines/checksums.sha256"
fi

if [ "$os" = "linux" ]; then
    cat > "$dist/run.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
exec /lib64/ld-linux-x86-64.so.2 --library-path "/usr/lib64:$PWD" "$PWD/tinynext" "$@"
EOF
    chmod +x "$dist/run.sh"
fi

out="$root/tinynext-v$version-$os-$arch.tar.gz"
rm -f "$out"
tar -C "$root" -czf "$out" dist
echo "produced: $out"
ls -lh "$out"
