# 岛屿卡片布局（island card layout）设计

日期：2026-08-06 · 状态：已批准 · 实现方式：方案 A

## 背景

当前 UI 只有**任务卡片**与**状态子侧边栏**是"岛"（圆角 surface 卡），内容区（工具栏
+ 任务列表 + 翻页）与设置页直接铺在页面背景上，左侧图标栏是整高 surface 竖条。
目标是把界面整体做成"岛屿卡片风"：所有区域都是浮在背景上的圆角卡片。

## 决策（已与用户确认）

1. **下载页内容区** → 整块一张大卡片（工具栏 + 列表 + 翻页都在卡内）。
2. **左侧图标栏** → 也浮起来成悬浮卡片。
3. **设置页** → 整块一张卡片（标题 + 表单 + 操作行）。
4. **所有卡片使用圆角**。
5. **实现方案 A**：新增可复用 `drawPanel()` 面板卡片函数，沿用绝对定位，页面内容按
   "卡片内偏移"微调坐标。不做容器化重构（方案 B）、不做纯装饰背景（方案 C）。

## 新增常量（`src/ui/utils.cppm`）

```cpp
export constexpr float kIslandGap = S(4.0f);     // 岛间距（实现后调小；注意：kCardGap 已用于任务卡间距）
export constexpr float kPanelPad = S(10.0f);     // 大卡内边距
export constexpr float kIslandRadius = S(10.0f); // 外层岛圆角
```

## 新增可复用函数（`src/ui/widgets.cppm`）

`drawPanel(ui, id, x, y, w, h, theme)`：画一张外层岛卡片背景：

- 颜色：`core::mixColor(background, surface, 0.5)`（**中间色调**，保证与纯 `surface`
  的任务卡分层，避免糊在一起）；
- 圆角：`kIslandRadius`；边框：`border` @0.6（与弹窗一致）；
- 投影：`shadow(S(14), S(3), 黑@0.25)`（暗色）/ `黑@0.12`（亮色）。

## 布局

### 全局外壳（`src/app.cpp`）

- 页面背景：全屏 `background` rect，保持不变。
- 图标栏 stack：从 `(0,0)`+`screen.height` 改为 `(kMargin, kMargin)` +
  `(kRailWidth, screen.height - 2·kMargin)`；内部 bg rect 填满 stack；
  logo/导航 y 相对 stack 顶部不变；**底部锚点**（关于 `screen.height-S(54)`、主题
  `screen.height-S(28)`）改为相对卡高 `cardH - S(54)` / `cardH - S(28)`。

### 下载页（`src/ui/pages.cppm::drawDownloadsPage`）

三岛横排，间距 `kIslandGap`：

- `railX = kMargin`，`subX = railX + kRailWidth + kIslandGap`，
  `contentX = subX + kSubSidebarWidth + kIslandGap`；
- `islandTop = kMargin`，`islandH = screen.height - 2·kMargin`。
- 状态子侧边栏卡：`(subX, islandTop)` size `(kSubSidebarWidth, islandH)`，底改
  `drawPanel`（圆角/色调统一），内容不变。
- 内容大卡：`(contentX, islandTop)` size `(contentW, islandH)`，`drawPanel` 打底；
  内部（pad = `kPanelPad`）：
  - 工具栏行 `toolY = islandTop + pad`，按钮右对齐到 `contentX + contentW - pad`；
  - 列表区：`listTop = toolY + kInputHeight + S(8)`，X = `contentX + pad`，
    宽 = `contentW - 2·pad`，`listHeight = pagerY - listTop - S(4)`；
  - 翻页行 `pagerY = islandTop + islandH - pad - kPagerHeight`，组水平居中于卡片内宽；
  - 状态消息：`pagerY - S(24)`，仍收在卡内；
  - 空态提示同理收进卡内。

### 设置页（`src/ui/pages.cppm::drawSettingsPage`）

- `contentX = railX + kRailWidth + kIslandGap`（无子侧边栏）；
- 一张 `drawPanel` 大卡 `(contentX, kMargin)` size `(contentW, screen.height-2·kMargin)`；
- pad = `kPanelPad`：标题 `islandTop+pad`、副标题、滚动表单（X 收进 pad），
  底部操作行钉在 `islandTop + islandH - pad - kActionH`。

### 卡片样式分层

- 外层岛（图标栏 / 子侧边栏 / 内容大卡 / 设置卡）：中间色调 + 圆角 `S(10)` +
  边框 0.6 + 柔和投影。
- 任务卡（内层，`cards.cppm`）：保持 `surface` + 圆角 `S(8)` + 边框 0.55，额外加
  极柔投影以和大卡分层。
- 弹窗（添加下载 / 关于）：已是卡片，保留，圆角统一 `S(10)`。

## 数据流 / 错误处理 / 测试

- 纯 UI 布局改动，无数据流、无引擎/配置变化。
- 验证：`mcpp build` 编译通过；视觉效果由用户 `mcpp run` 自行确认。
- 风险点：内容坐标偏移要仔细核对（工具栏/列表/翻页/空态/状态消息），防止贴边或
  越界；底部锚点（图标栏）随卡高变化。

## 实现后调整（用户反馈，2026-08-06）

1. **图标栏不套卡片**：恢复为整高 `surface` 竖条，从 `(0,0)` 占满窗口左缘（底部"关于/
   主题"锚回 `screen.height - S(54)/S(28)`）。浮岛卡片过多会把左侧显得零散，改为图标栏
   作左侧锚点、浮岛从它右侧起排。
2. **卡片与图标栏之间无间隙**：状态子侧边栏 / 设置卡起点 `kRailWidth`（贴住图标栏右缘），
   不再 `kRailWidth + kMargin`；子侧边栏与内容卡之间留 `kIslandGap`（已从 S(8) 逐次调小到 S(2)）。
3. **卡片与窗口顶部无间隙**：`islandTop = 0`、`islandH = screen.height`（顶部贴齐、整高），
   为后续无边框自定义顶部栏做准备——届时只需把 `islandTop` 改成标题栏高度。
4. **右边缘收窄并改名**：原 `kMargin`（S(12)）现在只用作卡片右边缘到窗口边的间隙，改名
   `kRightMargin` 并收窄到 S(6)，避免再被当作通用 margin 误用。
5. **总侧边栏透明**：左侧图标栏去掉整高 `surface` 底色矩形，logo/导航/底部按钮直接
   浮在页面背景上（主题背景透出更突出）；下载页的状态筛选子侧边栏**保留**独立岛卡片。
6. 追加修复：`openFile` / `openContainingFolder` 的 Windows 分支从 `std::system("explorer …")`
   改为 `ShellExecuteW`（共享 `shellExecFn()`），避免 UI 线程等 Explorer 退出而卡渲染。
