# 构建、运行与发布

修改构建、依赖、版本生成或打包流程，以及排查安装故障时阅读本文。

```bash
mcpp build          # 编译（dev）
mcpp build --release
mcpp run            # 启动 GUI（Linux 用 ./run.sh，见下）
.\make-dist.ps1     # Windows 发行打包 → dist\ + tinynext-v<版本>-win64.zip
bash make-dist.sh <os> <arch>   # Linux / macOS 打包 → tar.gz
```

- 工具链在 `mcpp.toml` 里固定为 `llvm@22.1.8`，不要改。
- 依赖版本锁定在 `mcpp.toml` / `mcpp.lock`，不要乱升：eui-neo **0.6.1**（配方加
  `-fno-char8_t` 修 C++23 构建 + 补 `-ldwmapi`，见 [roadmap](roadmap.md)）、
  websocket（IXWebSocket 包装）**12.0.1**、nlohmann::json **3.12.0**。
- **版本只在 `mcpp.toml` 维护**：应用版本 `[package].version` 与 eui/websocket/json
  依赖版本由 `scripts/gen-versions.ps1`（或 `.sh`）生成 `src/versions.generated.h`，
  `config.cppm` 的 `kAppVersion`/`kEuiVersion` 读它。升版本后跑一次生成脚本
  （`make-dist` 打包前会自动跑），不要手改生成头。
- **Linux 用 `./run.sh` 启动**而非 `mcpp run`：mcpp 私有 glibc 与系统 Mesa 的
  GLIBC 版本冲突，run.sh 走系统 ld.so + 系统 Mesa。
- **没有测试**：eui-neo 的 `app-main` 特性会把 `glfw_app_main.o` 急切链入，与任何
  定义 `main()` 的测试 TU 冲突（`multiple definition of 'main'`），tests/ 已删除，
  也不要再加测试可执行文件。
- 引擎二进制在 `engines/`（已 gitignore，`checksums.sha256` 保留）；缺失时下载
  不可用，打包脚本会警告但继续。
- **xlings 解压含 symlink 的 tarball 在 Windows 会中途失败**：若 `mcpp build` 报
  `install_packages failed` 而下载/校验正常，多半是包解压不完整（如 IXWebSocket 的
  `Dockerfile` symlink）。手工补装：`curl -L` 拉校验过的 tarball → 用系统 tar 解压
  （跳过 symlink）到 `~/.mcpp/registry/data/xpkgs/<ns>-x-<name>/<ver>/` +
  `mcpp_generated/`，再 `mcpp build`。
- CI：`.github/workflows/release.yml`。push `v*` 标签自动三平台构建 + **直接发布
  正式 Release**（draft: false，不留草稿）；`workflow_dispatch` 只构建上传
  artifacts（用于先修跨平台编译错误）。

## 提交与发布边界

**提交与发布**：改动在 feature 分支提交；验证通过后由助手直接 merge 到 `main`
并 push 代码（「用户自行 push、不要代 push」旧惯例已于 2026-09 作废）。
**不要因普通代码改动自动递增版本号、创建或推送 `v*` tag、发布
GitHub Release**；只有用户明确提出发布新版本时，才更新版本号、创建 tag 并发布。
