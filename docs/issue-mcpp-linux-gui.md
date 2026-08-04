## 环境

| 项 | 值 |
|---|---|
| 发行版 | Fedora 44 (x86_64),Wayland 会话 + XWayland(`DISPLAY=:0`) |
| mcpp | 2026.8.4.1(经 xlings 安装) |
| 工具链 | xim-x-llvm 22.1.8(默认) |
| 运行时 glibc | xim-x-glibc **2.39** |
| 相关包 | compat-x-glfw 3.4、compat-x-eui-neo 0.5.3(app-main)、compat-x-glx-runtime 2026.06.03 |
| 宿主图形栈 | 系统 Mesa 26.1.6(libGL/libGLX/libEGL,**要求 GLIBC_2.43**) |

## 现象

`mcpp build` 完全正常;`mcpp run` 后**进程立即退出、返回码 255(即 -1)、stdout/stderr 无任何输出**,窗口不出现。

## 最小复现

任何用到 GLFW + OpenGL 的 mcpp 项目都可复现,例如:

```toml
# mcpp.toml
[package]
name = "glfw-repro"
version = "0.1.0"

[targets.glfw-repro]
kind = "bin"
main = "src/main.cpp"

[dependencies.compat]
eui-neo = { version = "0.5.3", features = ["app-main"] }
```

`src/app.cpp` 只需定义 `app::dslAppConfig()` 和 `app::compose()`(eui-neo 的 GLFW 入口 `core/app/glfw_app_main.cpp` 提供 `main()`)。然后:

```bash
mcpp build   # 成功
mcpp run     # Running `target/.../bin/xxx` → 静默退出,exit code 255
```

## 诊断过程与根因

用项目 `target/` 里已编译的 glfw 对象文件链接一个带 `glfwSetErrorCallback` 的最小测试,逐层定位,发现**三个互相独立的问题叠加**:

### 问题 1:eui-neo/GLFW 初始化失败时完全静默(可观测性)

`glfw_app_main.cpp` 的 `main()` 在 `glfwInit()`、`createWindow()`、`createRenderBackend()`、`app::initialize()` 任一处失败都只 `return -1`,没有安装 GLFW error callback,也没有 stderr 输出,用户完全无法判断失败原因。

### 问题 2:compat-x-glx-runtime 包在 Fedora 系发行版上损坏(32 位符号链接)

GLFW 报 `GLX: Failed to load GLX`(65542)。`LD_DEBUG=libs` 显示 dlopen `libGLX.so.0` / `libGL.so.1` 时只搜索了 mcpp 自己的目录(二进制 RUNPATH + xlings 私有 ld.so.cache),**不含 `/usr/lib64`**,因此宿主 GL 库天然找不到。

mcpp 显然预见到了这一点 —— 二进制的 RUNPATH 里已经包含
`compat-x-glx-runtime/2026.06.03/mcpp_generated/glx_runtime/lib`。
但该包的内容是**指向 `/usr/lib/*` 的符号链接**:

```
libGLX.so.0       -> /usr/lib/libGLX.so.0        # Fedora 上这是 32 位库!
libGL.so.1        -> /usr/lib/libGL.so.1
libGLdispatch.so.0 -> /usr/lib/libGLdispatch.so.0
libGLX_mesa.so.0  -> /usr/lib/libGLX_mesa.so.0
libOpenGL.so.0    -> /usr/lib64/libOpenGL.so.0   # 只有这个是 64 位
```

Debian/Ubuntu 的 64 位库在 `/usr/lib/x86_64-linux-gnu` 或 `/usr/lib`,而 Fedora/RHEL/openSUSE 的 `/usr/lib` 是 **32 位库目录**,64 位库在 `/usr/lib64`。于是 dlopen 直接报:

```
libGLX.so.0: wrong ELF class: ELFCLASS32
```

即该包按 Debian 布局假设生成,在 Fedora 布局下整体失效。

### 问题 3(核心):mcpp 自带 glibc 2.39 与宿主图形栈的 glibc 要求不兼容

绕过问题 2(手工把 `/usr/lib64` 的 64 位库放进搜索路径)之后,所有 GL 库都能生成 link map,但 Mesa 在初始化时崩溃于:

```
libm.so.6: error: version lookup error: version `GLIBC_2.43' not found
(required by libgallium-26.1.6.so) (fatal)
```

mcpp 二进制的 `PT_INTERP` 是 `xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2`,进程内 glibc 固定为 2.39;而宿主的 Mesa/GLX/EGL 栈是按系统 glibc 2.43 编译的。**Linux 桌面 GUI 程序必须使用宿主机的 GL 驱动栈**(它还要和宿主 X/Wayland、DRM 设备通信),因此"自带 glibc 沙盒运行时"这个对 CLI 程序很友好的设计,在 GUI 场景下与宿主图形栈产生硬性版本冲突:宿主 Mesa 只会随发行版越来越新,自带 glibc 不升级就必然断。

另外实测 `LD_LIBRARY_PATH=/usr/lib64` 也**不能**作为变通:进程仍由 2.39 的 ld.so 加载,ld.so 与 libc.so.6 版本不匹配会直接段错误。

## 临时解决方案(workaround)

用**系统动态链接器**启动,让进程整体跑在系统 glibc 2.43 上(mcpp 编译产物按 2.39 编译,在 2.43 上向前兼容;自带的 X11 库也照常可用):

```bash
#!/usr/bin/env bash
# run.sh — 用系统 glibc 运行 mcpp 构建的 GUI 程序
set -euo pipefail
cd "$(dirname "$0")"
BIN=$(ls -d target/x86_64-linux-gnu/*/bin | head -1)
exec /lib64/ld-linux-x86-64.so.2 \
    --library-path "/usr/lib64:$PWD/$BIN" \
    "$PWD/$BIN/tinynext" "$@"
```

效果:GLX 初始化成功、窗口正常创建、中文渲染正常(已截图验证)。

## 修复建议

1. **compat-x-glx-runtime 包**:生成符号链接时不要假设 `/usr/lib` 是 64 位目录。建议改为探测 `ldconfig -p` 输出中的 64 位库实际路径,或按发行版布局选择 `/usr/lib64` / `/usr/lib/x86_64-linux-gnu`。
2. **glibc 版本策略(更根本)**:GUI 程序依赖宿主驱动栈,建议二选一:
   - 提供与主流发行版同步的新版 `xim-x-glibc`(≥ 2.43);或
   - 对 GUI 类目标提供"使用宿主 glibc 运行"的模式(例如 `mcpp run --system-glibc`,本质上就是上面的 workaround:用 `/lib64/ld-linux-x86-64.so.2 --library-path /usr/lib64:...` 启动,并把 `/usr/lib64` 注入 dlopen 搜索路径)。
3. **可观测性**:建议 compat-x-eui-neo 的 `glfw_app_main.cpp` 在 `main()` 开头安装 `glfwSetErrorCallback` 并向 stderr 打印错误;各初始化失败分支 `return -1` 前输出失败阶段名称。这一条能省掉用户 80% 的定位时间。

## 附:诊断中用到的关键命令

```bash
# 用项目已编译的 glfw 对象文件链接最小复现(带 error callback)
clang repro.c target/*/obj/compat_glfw/src/*.o -L$BINDIR -lX11 ... 
# → GLFW error 65542: GLX: Failed to load GLX

# 观察 dlopen 搜索路径(只有 mcpp 目录,没有 /usr/lib64)
LD_DEBUG=libs ./repro

# 验证 glx-runtime 包链接目标错误
ls -la ~/.mcpp/registry/data/xpkgs/compat-x-glx-runtime/*/mcpp_generated/glx_runtime/lib/
# → libGLX.so.0 -> /usr/lib/libGLX.so.0(32 位)

# 验证 glibc 版本鸿沟
llvm-readelf -V /usr/lib64/libgallium-26.1.6.so | grep GLIBC_  # 需要 2.43
strings ~/.mcpp/.../xim-x-glibc/2.39/lib64/libc.so.6 | grep GLIBC_ | tail -1  # 只有 2.39
```
