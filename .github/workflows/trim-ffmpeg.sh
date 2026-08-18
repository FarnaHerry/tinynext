#!/usr/bin/env bash
# trim-ffmpeg.sh — 从源码编译 TinyNext 需要的「瘦身静态 ffmpeg」。
#
# 用法: bash trim-ffmpeg.sh <Linux|macOS|Windows> <输出文件名: ffmpeg|ffmpeg.exe>
#
# 只编 DASH 音视频合并所需的最小集：-c:v copy -c:a copy|aac + mp4 输出。
# 全部 LGPL（--disable-gpl），无 libx264/libmp3lame，native aac 编码器内置。
# 产物 (~6-15MB) 输出到 engines/<文件名>。
#
# Windows 在 msys2 MINGW64 里跑（--target-os=mingw32 --extra-ldflags=-static）。
set -euo pipefail

os="${1:?usage: trim-ffmpeg.sh <Linux|macOS|Windows> <outname>}"
outname="${2:?usage: trim-ffmpeg.sh <os> <outname>}"

# 钉一个已知稳定的 ffmpeg 版本（研究确认 n9.0 是 2025 底的最新稳定，6.1 更成熟；
# 用 7.1 长期版，稳定且编码器齐全）。
ffver="7.1"
work="$RUNNER_TEMP/ffmpeg-src"   # 编译工作目录
mkdir -p "$work"

# 官方源码 tar 包（ffmpeg.org 分平台无差异，直接下载）。
cd "$work"
curl -fL --retry 3 -o ffmpeg.tar.xz \
  "https://ffmpeg.org/releases/ffmpeg-${ffver}.tar.xz"
tar -xJf ffmpeg.tar.xz
cd "ffmpeg-${ffver}"

# 共用最小 configure：DASH 合并 = demux mov/mp4/matroska/webm + 原生解码（copy 模式
# 虽不解码，但兜底转码需几个常见 decoder）+ mp4 mux + aac encoder + 必要 bsf。
# decoder 多配几个（bilibili h264/aac、YouTube 常有 vp9/av1/opus/vorbis），
# 防个别流需转码时缺解码器。体积代价很小。
common=(
  --disable-everything
  --disable-gpl
  --disable-doc
  --disable-debug
  --disable-programs
  --disable-avdevice
  --disable-postproc
  --disable-network
  --enable-small
  --enable-ffmpeg
  --enable-avcodec
  --enable-avformat
  --enable-avutil
  --enable-swresample
  --enable-swscale
  --enable-protocol=file
  --enable-demuxer=mov,mp4,matroska,webm
  --enable-decoder=h264,hevc,vp9,av1,aac,mp3,vorbis,opus,flac,tak
  --enable-encoder=aac
  --enable-muxer=mp4
  --enable-bsf=aac_adtstoasc
  --enable-bsf=extract_extradata
)

extra=()
if [ "$os" = "Windows" ]; then
  # msys2 MINGW64：交叉静态编译，静态 glibc 后不依赖运行库。
  extra+=(--arch=x86_64 --target-os=mingw32 --extra-cflags="-static" --extra-ldflags="-static")
elif [ "$os" = "macOS" ]; then
  extra+=(--arch=arm64 --target-os=darwin --cc=clang)
# Linux: 原生 gcc，无 extra。
fi

./configure "${common[@]}" "${extra[@]}"

# 并行编译只出 ffmpeg 程序（avcodec/avformat 等 lib 作为依赖自动编，-j 并行）。
# 并行编译（跨平台并行数）：nproc 只有 Linux 有，macOS 用 sysctl，msys2 下取
# NUMBER_OF_PROCESSORS 环境变量。ffmpeg 的 Makefile 无独立 `ffmpeg` target（主
# 目标是 `all`，经 PROGS 生成 `ffmpeg`/`ffmpeg_g`），直接给 target 名会报
# "No rule"，所以用默认 all 目标（--disable-programs 后只编 ffmpeg 程序）。
if [ "$(uname)" = "Darwin" ]; then
    njobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then
    njobs="$NUMBER_OF_PROCESSORS"   # Windows/msys2
else
    njobs="$(nproc 2>/dev/null || echo 4)"
fi
make -j"$njobs"

# 产物拷到 actions 期望的位置。mingw/msys 下程序名带 .exe，unix 无后缀。
mkdir -p "$GITHUB_WORKSPACE/engines"
out_src="ffmpeg"
[ "$os" = "Windows" ] && out_src="ffmpeg.exe"
cp -f "./$out_src" "$GITHUB_WORKSPACE/engines/$outname"

echo "=== 产物大小 ==="
ls -lh "$GITHUB_WORKSPACE/engines/$outname"
