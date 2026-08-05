# AGENTS.md — 给 AI 助手的项目指南

TinyNext 是一个 **C++23 模块化 GUI 下载器**：EUI-NEO 前端 + 可切换下载引擎
（内置 tinyhttps / 外部 aria2-next），单实例，带命令行传参。跨平台
（Windows / Linux / macOS），用 **mcpp** 构建。

## 构建 / 运行

```bash
mcpp build          # 编译（dev）
mcpp build --release
mcpp run            # 启动 GUI（Linux 用 run.sh）
```

- 工具链在 `mcpp.toml` 里固定为 `llvm@22.1.8`，不要改。
- Windows 发行打包：`.\make-dist.ps1`；Linux/macOS：`bash make-dist.sh <os> <arch>`。
- CI：`.github/workflows/release.yml`，push `v*` 标签自动三平台构建 + 发布。

## 用 CLI 添加下载（AI 最常用）

```bash
tinynext https://example.com/file.zip     # 添加下载；应用没开会自动启动
tinynext url1 url2                         # 一次多个
tinynext agent                             # 打印 CLI 使用教学（给 AI 用），退出
```

- **单实例**：重复启动不弹新窗口——第二实例把 URL 转发给已运行实例（写
  `<temp>/tinynext.inbox`，Windows 上还会聚焦窗口）后退出。
- 只有 `http://` / `https://` 开头的参数会被当作下载；非 URL 参数忽略。
- `agent` / `--agent` / `help` 参数会打印 CLI 使用教学并退出（不进 GUI）——AI
  不知道用法时先跑 `tinynext agent`。
- 详细：`docs/cli.md`。

## 代码结构（全模块）

| 模块 | 文件 | 职责 |
|------|------|------|
| `tinynext.download_engine` | `src/download_engine.cppm` | 引擎接口 `dl::DownloadEngine` |
| `tinynext.download_manager` | `src/download_manager.cppm/.cpp` | tinyhttps 引擎 |
| `tinynext.aria2_engine` | `src/aria2_engine.cppm/.cpp` | aria2-next 引擎（JSON-RPC） |
| `tinynext.config` | `src/config.cppm` | 配置 / 主题 / 下载目录 |
| `tinynext.cli` | `src/cli.cppm` | 单实例 + 命令行 URL + inbox |
| `tinynext.ui.*` | `src/ui/*.cppm` | utils / theme / state / platform / widgets / cards / pages |
| `src/app.cpp` | 普通 TU | 入口：`app::dslAppConfig()` + `app::compose()` |

## 关键约定（改代码前必读）

1. **入口**：`main()` 由 eui-neo 的 `app-main` 提供，任何 TU 都不能再定义 `main()`。
2. **`import std;` 后禁止再 `#include` 标准头**（std 模块已声明）。
3. **eui_neo.h 是 header-only 无模块接口**：`src/app.cpp` 包含完整 `<eui_neo.h>`
   （提供 `dsl_app_impl.h` 里的 `app::update/render` 机制）；**UI 模块只包含精简头
   `src/ui/eui_ui.h`**（去掉 `dsl_app_impl.h`）——否则内联 lambda 会 mangled name
   冲突。给 UI 模块加 include 时用 `"eui_ui.h"`。
4. **共享状态**：所有可变 UI 全局在 `tinynext.ui.state` 模块（导出，直接读写）。
   引擎对象是 `state::g_manager`（`unique_ptr<dl::DownloadEngine>`）。
5. **每任务连接数**：`dl::StartOptions{connections}`，aria2 生效，tinyhttps 忽略。
6. **缩放**：所有尺寸经 `utils::S(x)`（`kUI=1.4`）放大，不要写裸像素。
7. **aria2 引擎**：进程名 Windows 是 `aria2-next.exe`，unix 是 `aria2-next`；
   字段名用 `connections`（不是 `numConnections`）。
8. **提交**：本地 commit 后由用户自行 push（不要代 push）。
