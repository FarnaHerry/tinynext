# TinyNext 视觉、布局与缩放

修改主题、页面、弹窗、任务卡、字体或尺寸时阅读本文。

## 视觉与任务卡

**黑白极客视觉体系**：界面是纯中性灰 monochrome 风（对标 Codex / Claude Code
页面）——亮暗双主题严格镜像，**主色 = 反白**（暗主题 primary=白/onPrimary=黑，
亮主题反转）。层次靠 **1px hairline 描边**（`kHairline` + `theme.outline`）+
灰阶梯（surfaceContainerLow..Highest）表达，**扁平无投影**——唯一例外是弹窗
（`shadow(16,4)`：dark 黑 α0.40 / light 黑 α0.12）。色板全部在
`ui/theme.cppm` 的 `AppTheme`（字段名沿用 M3 角色名，语义已是单色体系）+
语义容器层 **`panelBg`（岛卡底）/ `cardBg`（内容卡底）**。状态色只保留哑化
绿/红（done/failed），活动态用单色灰。交互叠加色用 `stateLayer(onColor, α)`
（hover 0.08 / pressed 0.12）。弹窗统一规范：scrim α0.5 遮罩 +
surfaceContainerLow + hairline + `kDialogRadius`(8) + 那套弹窗投影；次要按钮用
`widgets::drawTextButton`。岛卡 = `widgets::drawPanel`（panelBg + hairline +
`kIslandRadius`=8）；圆角令牌 `kCardRadius`=6 / `kChipRadius`=4 /
`kButtonRadius`=6（pill 已退役）。**等宽字体点缀**：`kMonoFont`
（JetBrains Mono，assets/ 内置，OFL）用于数字/速度/页码/版本号等纯拉丁片段，
CJK 靠 eui 字体栈回退（`fontFamily` 带 `.` 按项目资产路径加载）。

**任务卡进度条统一竖向**（用户明确偏好，2026-10-11）：放在卡片左侧，自下向上
填充并贯穿卡片内容高度；进度轨道和刻度独占左列，右列沿用第一版的两行文件名、
来源、大号百分比、状态与传输信息；所有任务状态使用同一布局。
未知总量显示 `--`、直播显示 `LIVE`，不伪造进度。后续布局精简应保留竖向风格。

**任务卡操作独占右列最后一行**：不与上方信息并排，不侵入左侧进度列，
不添加操作区分隔线；按钮组左对齐，保持既有阅读顺序，不向上换行，窄卡片只收紧
按钮间距，保留按钮大小。两列样式保持紧凑，卡片高度由 `kCardHeight` 统一管理，
宽度限制在 `kTaskCardMinWidth` / `kTaskCardMaxWidth` 范围内。

## 缩放

**缩放**：eui-neo 0.5.6 起 `DslAppConfig::uiScale(kUI)` 原生放大（布局+字号）；
尺寸按设计逻辑像素直接写，不再 `S()` 自乘；窗口物理尺寸 = 设计尺寸 × `kUI`
（`kUI=1.4`，`app.cpp` 里 `windowSize(1120*kUI, 720*kUI)`）。`kUI` 仍是唯一
缩放旋钮。

窗口布局与高 DPI 最小画布的既有决策见
[窗口布局文档](../../../docs/window-layout.md)。
