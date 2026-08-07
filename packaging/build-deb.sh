#!/usr/bin/env bash
# build-deb.sh - stage a .deb from dist/ (created by make-dist.sh).
#
# Usage: bash packaging/build-deb.sh <version>
#
# Layout:
#   /opt/tinynext/       tinynext binary + assets/ + engines/ (from dist/)
#   /usr/bin/tinynext    launcher -> system loader (Debian + Fedora lib paths)
#   /usr/share/applications/tinynext.desktop
#   /usr/share/icons/hicolor/256x256/apps/tinynext.png
#
# The launcher deliberately does NOT `cd /opt/tinynext`: config (tinynext.conf)
# is written to the cwd, so a desktop launch (cwd=$HOME) writes it to $HOME.

set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:?usage: build-deb.sh <version>}"
dist="$root/dist"
[ -d "$dist" ] || { echo "dist/ missing - run make-dist.sh first" >&2; exit 1; }

work="$root/.deb-root"
rm -rf "$work"
mkdir -p "$work/DEBIAN" "$work/opt/tinynext" "$work/usr/bin" \
         "$work/usr/share/applications" \
         "$work/usr/share/icons/hicolor/256x256/apps"

# ---- control ----
cat > "$work/DEBIAN/control" <<EOF
Package: tinynext
Version: $version
Section: net
Priority: optional
Architecture: amd64
Maintainer: FarnaHerry <farnaherry@users.noreply.github.com>
Depends: libc6 (>= 2.39), libgl1
Description: GUI downloader (EUI-NEO frontend + aria2-next engine)
 Cross-platform downloader with segmented multi-connection downloads,
 resume, magnet/BT. Requires a desktop with Mesa/GLX and glibc >= 2.39.
EOF

# ---- /opt/tinynext ----
cp -r "$dist/." "$work/opt/tinynext/"
chmod +x "$work/opt/tinynext/tinynext"
if [ -x "$work/opt/tinynext/engines/aria2-next" ]; then
    chmod +x "$work/opt/tinynext/engines/aria2-next"
fi

# ---- launcher (先 cd 到安装目录：ld.so 拉起时 /proc/self/exe 指向 loader，eui 靠
#      CWD 的 assets/ 解析字体/图标；配置读写走 XDG，不受 CWD 影响) ----
cat > "$work/usr/bin/tinynext" <<'EOF'
#!/bin/sh
cd /opt/tinynext
exec /lib64/ld-linux-x86-64.so.2 \
  --library-path "/usr/lib/x86_64-linux-gnu:/usr/lib64:/opt/tinynext" \
  /opt/tinynext/tinynext "$@"
EOF
chmod 755 "$work/usr/bin/tinynext"

# ---- desktop entry ----
cat > "$work/usr/share/applications/tinynext.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=TinyNext
Comment=GUI downloader
Exec=/usr/bin/tinynext
Icon=tinynext
StartupWMClass=TinyNext 下载器
Terminal=false
Categories=Network;FileTransfer;GTK;
EOF

# ---- icon (pre-rendered 256x256 PNG, committed as assets/icon.png) ----
# 不用 imagemagick 现场转：icon.ico 是多帧，convert 会输出 tinynext-0.png 而非
# tinynext.png。直接拷仓库里的 icon.png。
if [ -f "$root/assets/icon.png" ]; then
    cp "$root/assets/icon.png" "$work/usr/share/icons/hicolor/256x256/apps/tinynext.png"
else
    echo "WARN: assets/icon.png missing - desktop icon skipped" >&2
fi

# ---- md5sums (paths relative to package root, no leading ./) ----
(cd "$work" && find . -type f ! -path './DEBIAN/*' -exec md5sum {} + \
    | sed 's|^\([0-9a-f]*\)  \./|\1  |' > DEBIAN/md5sums)

out="$root/tinynext-v$version-linux-x86_64.deb"
rm -f "$out"
dpkg-deb --build --root-owner-group "$work" "$out" >/dev/null
echo "produced: $out"
