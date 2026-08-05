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

if [ -x "$root/engines/aria2-next" ]; then
    cp "$root/engines/aria2-next" "$dist/engines/aria2-next"
    chmod +x "$dist/engines/aria2-next"
else
    echo "WARN: engines/aria2-next missing — release has only the built-in engine" >&2
fi

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
