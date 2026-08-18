# Third-Party Notices

TinyNext 随发行包附带以下第三方程序（均在 `engines/` 目录），各自独立分发并受
其原始许可约束。TinyNext 本体为 MIT 许可（见 `LICENSE`），但**这些第三方二进制不
属于 TinyNext 的 MIT 代码**，请按各自许可使用/再分发。完整许可文本请访问下列链接
获取原文，本文仅为来源与许可说明。

---

## aria2-next

- **版本**：2.5.5
- **用途**：TinyNext 的下载引擎（JSON-RPC 驱动，外部进程）。
- **来源**：https://github.com/AnInsomniacy/aria2-next
- **许可**：GPLv2（GNU General Public License v2）
- **说明**：aria2 家族项目，随发行包单独分发。完整 GPLv2 文本见
  https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt 及其仓库 LICENSE。
- **二进制**：`engines/aria2-next.exe`

## yt-dlp

- **版本**：2026.07.04
- **用途**：视频网页解析（出直链 / 请求头），并在 YouTube 等站点承担原生下载+合并。
- **来源**：https://github.com/yt-dlp/yt-dlp
- **许可**：Unlicense（公有领域），并保留对上游 youtube-dl 的版权声明。
- **说明**：yt-dlp 派生自已停止维护的 youtube-dl。正式许可文本见仓库 LICENSE：
  https://github.com/yt-dlp/yt-dlp/blob/master/LICENSE
- **二进制**：`engines/yt-dlp.exe`

## ffmpeg

- **版本**：滚动 latest（BtbN FFmpeg-Builds master-latest，全平台统一）
- **用途**：DASH 音视频流合并 / 视频转码。
- **来源**：https://ffmpeg.org ；全平台统一通过 BtbN/FFmpeg-Builds（GitHub Releases）
  获取。各平台对应 asset：
  - Windows win64：`ffmpeg-master-latest-win64-lgpl.zip`（~18MB 单 binary）
  - Linux x64：`ffmpeg-master-latest-linux64-lgpl.tar.xz`（~15MB 单 binary）
  - macOS arm64：evermeet.cx universal binary（~26MB 单 binary，比 BtbN 的
    macos-universal-gpl 小一半以上）
- **许可**：**LGPL-2.1+**（BtbN LGPL 构建不含 GPL-only 的 x264/x265 等编码器；
  TinyNext 只用到 demuxer + `-c:v copy` / `-c:a copy|aac`，不需要任何 GPL
  编码器。FFmpeg 本体源码可依 LGPL-2.1+ 或 GPL 选择）。
- **说明**：如果你的使用场景依赖 GPL 编码器（x264/x265 等），请从提供 GPL 构建
  的渠道获取并自行替换 `engines/ffmpeg(.exe)`。官方许可文本见 https://ffmpeg.org/legal.html
- **二进制**：`engines/ffmpeg`（unix）/ `engines/ffmpeg.exe`（Windows）

---

本文件随 TinyNext 发行包分发；更多关于 TinyNext 自身许可的信息见项目 `LICENSE`。
