# AGENTS.md — TinyNext 项目指南

TinyNext 是 C++23 模块化 GUI 下载器：EUI-NEO 前端、aria2-next 外部进程引擎，
支持 Windows / Linux / macOS、单实例和命令行传参，使用 mcpp 构建。
aria2-next 是唯一引擎，TinyHttpsEngine 已移除。

## 指南维护规则

- 根 `AGENTS.md` 同时满足 **最多 150 行、6,000 字符**，字符数包含空白和换行。
- 单个项目内 `SKILL.md` 最多 **100 行**；长细节按需引用。
- 项目技能放在 `project-skills/<技能名>/SKILL.md`，独立维护并纳入版本管理。
- 技能长细节放在该技能的 `references/`，或引用已有专题文档，避免重复维护。
- 项目级参考文件必须存放在本项目内、独立维护并纳入版本管理。
- 超限先合并重复内容或外移，禁止压成长行或删除必要约束规避。
- 本文件维护全局规则与阅读入口，专题文件维护对应细节；修改约定应更新其归属文件。
- `CLAUDE.md` 仅引用本文件，不维护第二套规则。

## 按任务读取

开始相关修改前，必须读取对应入口；技能中的参考文件按具体任务读取。

| 修改范围 | 必读入口 |
| --- | --- |
| UI 页面、弹窗、控件、布局与交互 | [TinyNext UI skill](project-skills/tinynext-eui-ui/SKILL.md) |
| 模块边界、store、共享状态、页面职责 | [架构与状态归属](docs/architecture.md) |
| 构建、依赖、版本生成、打包与安装故障 | [构建与发布](docs/build-release.md) |
| 下载行为、daemon、RPC、WS 与引擎锁 | [aria2 引擎](docs/aria2-engine.md) |
| 句柄、socket、动态库、子进程、后台线程 | [资源生命周期](docs/resource-lifecycle.md) |
| CLI、单实例、headless 与扩展宿主 | [CLI 文档](docs/cli.md) |

功能说明见 [README](README.md)，历史决策与验证见 [roadmap](docs/roadmap.md)。

## 构建与运行

```bash
mcpp build                 # 开发构建
mcpp build --release       # 发布构建
./run.sh                   # Linux 启动 GUI
mcpp run                   # Windows / macOS 启动 GUI
```

- 工具链固定 `llvm@22.1.8`；依赖锁定在 `mcpp.toml` / `mcpp.lock`，不要随意升级。
  当前为 eui-neo **0.6.1**、websocket **12.0.1**、nlohmann::json **3.12.0**。
- 版本只在 `mcpp.toml` 维护；变更后运行 `scripts/gen-versions.sh` 或 `.ps1`。
  `src/versions.generated.h` 是生成文件，禁止手改。
- Linux 必须通过 `./run.sh` 走系统 ld.so 与 Mesa，规避 mcpp 私有 glibc 冲突。
- 项目没有测试可执行文件，也不要新增：eui 的 `app-main` 会急切链入入口对象，
  与测试 TU 的 `main()` 冲突。验证使用构建和与改动相关的实际运行检查。
- 引擎二进制位于已忽略的 `engines/`；缺失时无法下载，打包只警告后继续。

## CLI 快速入口

```bash
tinynext agent                            # 打印教学并退出，不进 GUI
tinynext https://example.com/file.zip      # 未运行时自动启动，已运行时转发
tinynext url1 url2                         # 一次添加多个
```

- 不熟悉 CLI 时先运行 `tinynext agent`；`--agent` / `help` 也会打印教学并退出。
- 单实例通过 loopback TCP 转发，失败回退 `<temp>/tinynext.inbox`，重复启动不新开窗口。
- 下载源白名单只在 `utils::isDownloadableSource` 维护，非下载参数忽略。
  支持 HTTP(S)、SFTP、ED2K file、magnet 与本地 `.torrent`（aria2-next 2.8.3）。
- 命令行解析只用 `utils::commandLineArgs()`，CLI 与 headless 共用；
  Linux ld.so 入口须剥加载器选项再跳过程序路径，禁止另写仅看 argv[0] 的解析器。

## 全局代码约束

- `main()` 由 eui-neo `app-main` 提供，任何 TU 都不能另定义。
  `src/app.cpp` 只提供 `app::dslAppConfig()` 与 `app::compose()`。
- `import std;` 后禁止再包含标准头。UI 模块使用 `"eui_ui.h"`；
  `src/app.cpp` 包含完整 `<eui_neo.h>` 取声明。
- `store.tasks` 管领域任务与引擎，不 import UI/eui；`startFromUrl` 返回
  `StartResult{ok, message}`，不做 UI 提示。视图与弹窗状态属于 `store.ui` /
  `store.dialogs`；设置 pending 草稿归设置页私有，主题归 `ui.theme`。
- 禁止在 compose 中挂 `.onFrame`。周期工作放在有 owner 的后台线程，
  仅有实际变化时 `core::platform::requestUiUpdate()` 唤醒 UI。
  空闲必须保持 `glfwWaitEvents` 睡眠、零渲染零 CPU。
- 原生资源获取后立即交给 move-only RAII owner；优先复用 `src/native_resource.hpp`，
  释放 API 必须匹配来源，借用句柄不能重复关闭，所有权转交用 move 或 `.release()`。
- 每个后台线程必须有停止和 join 的 owner，优先 `std::jthread` + `stop_token`。
  阻塞线程必须可唤醒；先停止并 join worker，再销毁其依赖的对象、DLL 或引擎。
  新代码默认禁止 `detach()`，例外的生命周期证明要求见资源生命周期文档。
- 保持黑白中性灰、亮暗镜像主题。任务卡统一左列竖向进度，操作独占右列最后一行；
  输入框统一 `widgets::outlinedInput`。详细布局与令牌按 UI skill 的参考文件执行。
- EUI 元素 ID 在整棵树中必须唯一、稳定；动态项及回调使用业务 key。
  测量型 content 回调必须用参数提供的 `eui::Ui&` 构建子项。
- aria2-next 没有下载级 priority，不得重新加入优先级功能。
  引擎锁顺序为 `daemonMutex_` → `tasksMutex_`，具体持锁边界见引擎文档。

## 提交与发布

- 改动在 feature 分支提交；验证通过后由助手直接 merge 到 `main` 并 push 代码。
- 普通改动不自动递增版本、不创建或推送 `v*` tag、不发布 GitHub Release。
  只有用户明确要求发布新版本时才进行这些操作。
- `v*` tag 会触发三平台构建并直接发布正式 Release；
  `workflow_dispatch` 仅构建并上传 artifacts，可用于发布前排查跨平台问题。
