# tinynext.spec - RPM spec for TinyNext (Linux x86_64).
# Built by build-rpm.sh; the repo path and app version are injected via rpmbuild
# --define "_repo <abs path>" / --define "appver <version>". dist/ (created by
# make-dist.sh) is the payload.
#
# The launcher executes through the SYSTEM glibc loader (the mcpp toolchain
# links the binary against its private glibc). --library-path covers Fedora
# (/usr/lib64) and Debian multiarch, plus the app dir.

Name:           tinynext
Version:        %{appver}
Release:        1
Summary:        GUI downloader (EUI-NEO frontend + aria2-next engine)

License:        MIT
URL:            https://github.com/FarnaHerry/tinynext
BuildArch:      x86_64

%description
Cross-platform downloader with segmented multi-connection downloads, resume,
magnet/BT. Requires a desktop with Mesa/GLX and glibc >= 2.39.

%prep

%build

%install
rm -rf %{buildroot}
install -d %{buildroot}/opt/tinynext %{buildroot}/usr/bin \
         %{buildroot}/usr/share/applications \
         %{buildroot}/usr/share/icons/hicolor/256x256/apps
cp -r %{_repo}/dist/. %{buildroot}/opt/tinynext/
chmod +x %{buildroot}/opt/tinynext/tinynext
if [ -x %{buildroot}/opt/tinynext/engines/aria2-next ]; then
    chmod +x %{buildroot}/opt/tinynext/engines/aria2-next
fi
# 视频依赖同样给执行位（make-dist.sh 已在 dist 里 +x，这里兜底）。
for name in yt-dlp ffmpeg; do
    if [ -x %{buildroot}/opt/tinynext/engines/$name ]; then
        chmod +x %{buildroot}/opt/tinynext/engines/$name
    fi
done

cat > %{buildroot}/usr/bin/tinynext <<'EOF'
#!/bin/sh
# 先 cd 到安装目录：经系统 ld.so 拉起时 /proc/self/exe 指向 loader（而非本程序），
# eui 只能靠 CWD 下的 assets/ 解析字体/图标（与 make-dist.sh 生成的 run.sh 一致）。
# 配置读写走 $XDG_CONFIG_HOME（configPath 优先 XDG），不受 CWD 影响。
cd /opt/tinynext
exec /lib64/ld-linux-x86-64.so.2 \
  --library-path "/usr/lib64:/usr/lib/x86_64-linux-gnu:/opt/tinynext" \
  /opt/tinynext/tinynext "$@"
EOF
chmod 755 %{buildroot}/usr/bin/tinynext

cat > %{buildroot}/usr/share/applications/tinynext.desktop <<'EOF'
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

# 预渲染 PNG（icon.ico 是多帧，convert 会输出 tinynext-0.png，这里直接拷 icon.png）
cp %{_repo}/assets/icon.png \
    %{buildroot}/usr/share/icons/hicolor/256x256/apps/tinynext.png

%files
/opt/tinynext
/usr/bin/tinynext
/usr/share/applications/tinynext.desktop
/usr/share/icons/hicolor/256x256/apps/tinynext.png

%post
# best-effort: register the desktop entry
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
