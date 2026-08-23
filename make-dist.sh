#!/usr/bin/env bash
# make-dist.sh — stage dist/ and tar.gz it for unix (Linux / macOS) releases.
# Windows counterpart: make-dist.ps1.
#
# Usage: bash make-dist.sh <linux|macos> <x86_64|arm64>
#
# Linux note: the mcpp toolchain links the binary against its private glibc
# (PT_INTERP points into the mcpp install). A bare copy won't run on other
# machines, so we ship a `run.sh` launcher that executes through the SYSTEM
# loader with /usr/lib64 first — same trick as the repo-root run.sh. This
# requires the target distro to have glibc >= the build's requirement
# (mcpp toolchain is glibc 2.39; system Mesa may need newer). macOS binaries
# use the system loader and need no launcher.

set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
os="${1:?usage: make-dist.sh <linux|macos> <x86_64|arm64>}"
arch="${2:?usage: make-dist.sh <linux|macos> <x86_64|arm64>}"
version="$(grep -m1 '^version' "$root/mcpp.toml" | sed -E 's/.*"([^"]+)".*/\1/')"

# 重新生成版本头（src/versions.generated.h）：版本只在 mcpp.toml 维护。
bash "$root/scripts/gen-versions.sh"

# Newest built target dir: target/<triple>/<hash>/bin
build_bin="$(ls -dt "$root"/target/*/*/bin 2>/dev/null | head -1)"
if [ -z "$build_bin" ] || [ ! -x "$build_bin/tinynext" ]; then
    echo "binary not found — run \`mcpp build --release\` first" >&2
    exit 1
fi
echo "  binary: $build_bin/tinynext"

dist="$root/dist"
rm -rf "$dist"
mkdir -p "$dist/engines"

cp "$build_bin/tinynext" "$dist/tinynext"
chmod +x "$dist/tinynext"
cp -r "$root/assets" "$dist/assets"
# 第三方二进制（aria2-next / yt-dlp / ffmpeg）的许可与来源声明，随包带到根目录。
if [ -f "$root/THIRD-PARTY-NOTICES.md" ]; then
    cp "$root/THIRD-PARTY-NOTICES.md" "$dist/THIRD-PARTY-NOTICES.md"
fi

if [ -x "$root/engines/aria2-next" ]; then
    cp "$root/engines/aria2-next" "$dist/engines/aria2-next"
    chmod +x "$dist/engines/aria2-next"
    # 运行时完整性校验清单：随包分发，保证 spawn 前能校验二进制未被篡改
    # （Windows 的 make-dist.ps1 直接拷整个 engines/ 已含它）。
    cp "$root/engines/checksums.sha256" "$dist/engines/checksums.sha256"
else
    echo "WARN: engines/aria2-next missing — aria2-next is the only engine, downloads will not work" >&2
fi

# yt-dlp / ffmpeg：视频解析 + DASH 合并依赖。缺失只影响视频功能（普通下载不受影响），
# 所以只警告不报错；它们更新频繁，运行时不做完整性校验，随包分发即可。
for name in yt-dlp ffmpeg; do
    if [ -x "$root/engines/$name" ]; then
        cp "$root/engines/$name" "$dist/engines/$name"
        chmod +x "$dist/engines/$name"
    else
        echo "WARN: engines/$name missing — video parse/merge will not work" >&2
    fi
done

if [ "$os" = "linux" ]; then
    # 同根目录 run.sh：走系统 loader，优先 /usr/lib64 的系统 Mesa/glibc。
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
