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

- **版本**：滚动 latest（Windows gyan.dev essentials 9.0.1 起）
- **用途**：DASH 音视频流合并 / 视频转码。
- **来源**：https://ffmpeg.org ；按平台构建渠道不同：
  - Windows：https://www.gyan.dev/ffmpeg/builds/（essentials build）
  - Linux x64：https://github.com/BtbN/FFmpeg-Builds（master-latest linux64-gpl）
  - macOS arm64：https://evermeet.cx/ffmpeg/
- **许可**：**GPL**（gyan / BtbN 构建内含 x264 / x265 等 GPL 组件，因此整体按 GPL
  授权；FFmpeg 本体源码可依 LGPL-2.1+ 或 GPL 选择）。
- **说明**：如果你的使用场景需要严格的 LGPL 版本，请从提供 LGPL 构建的渠道获取
  并自行替换 `engines/ffmpeg(.exe)`。官许可文本见 https://ffmpeg.org/legal.html
- **二进制**：`engines/ffmpeg`（unix）/ `engines/ffmpeg.exe`（Windows）

---

本文件随 TinyNext 发行包分发；更多关于 TinyNext 自身许可的信息见项目 `LICENSE`。
