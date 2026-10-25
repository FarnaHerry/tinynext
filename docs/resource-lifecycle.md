# 原生资源与线程生命周期

涉及句柄、socket、管道、动态库、子进程或后台线程时阅读本文。

## 原生资源

**原生资源一律用 RAII 管理**（全项目强制）：获取资源后立刻交给 move-only
所有者，不要把裸句柄留到函数末尾再手工清理，也不要给多个 `return` 分支
分别配平。通用包装在 `src/native_resource.hpp`，按资源使用正确的释放 API：

- Windows `HANDLE` → `native::UniqueHandle` / `CloseHandle`；模块 →
  `UniqueModule` / `FreeLibrary`；`LocalAlloc` → `UniqueLocalAlloc` /
  `LocalFree`；`CoTaskMem` → `UniqueCoTaskMem` / 对应的 `CoTaskMemFree`；
  自有 `HICON` → `UniqueIcon` / `DestroyIcon`。共享系统图标（如
  `LoadIconW(nullptr, ...)`）是借用资源，不要交给 `UniqueIcon`。
- POSIX fd → `UniqueFd` / `close`；`popen` → `UniqueFile` / `pclose`；
  socket 用具备析构关闭的 socket owner（如 `LocalSocket`、`CliSocket`）。
  `WSAStartup`、IXWebSocket 网络初始化等成对 API 也要有 session owner，
  确保成功初始化才执行对应 cleanup。
- 子进程 owner 除释放 Windows 进程句柄外，还要负责需要退出的子进程
  terminate + wait；POSIX 子进程要 wait/reap，不能只丢弃 PID。未完成启动、
  超时、异常及重复 shutdown 都必须安全收敛。`CreateProcessW` 用显式继承句柄
  列表；POSIX fd 默认设 `FD_CLOEXEC`，只把标准输入/输出等明确需要的 fd
  通过 spawn actions 交给子进程。
- 已有 owner 可直接复用；新增类型也遵循 delete-copy、支持 move、析构释放。
  `.get()` 只借用，所有权转交必须使用 `.release()` 或 move，并在接收处立刻
  建立新 owner。借用句柄不要重复关闭。释放函数要匹配资源来源，不能用
  `CloseHandle` 释放图标、socket 或 `LocalFree` 内存。
- 在资源创建点检查：每个 `Create*` / `LoadLibrary` / `socket` / `open` /
  `pipe` / `popen` / `spawn` 是否立即进入 owner；每条提前退出、异常路径和
  子进程继承路径是否仍正确释放。文件动作列表等需要显式 `destroy` 的 C API
  也应使用局部 guard。

## 后台线程

**线程也是资源**：每个后台线程都要有负责停止和 join 的 owner。优先用
`std::jthread` + `stop_token`；使用 `std::thread` 时由模块/对象保存并 join，
阻塞线程必须有可关闭的 socket、stop pipe、条件变量或消息队列来唤醒。退出时
按依赖顺序先停止并 join 访问某对象的 worker，再销毁该对象或它依赖的 DLL/引擎；
`atexit` 回调按后注册先执行，注册顺序必须和资源依赖相符。禁止 detach 捕获
局部对象、模块静态对象或仍可能析构的单例。新代码默认禁止 `detach()`；确有
进程末尾的一次性工作需要例外时，必须先证明所有捕获对象和被调用模块在 join
前仍存活，并在代码注释说明依据，不能仅以“全局单例”作为生命周期证明。
