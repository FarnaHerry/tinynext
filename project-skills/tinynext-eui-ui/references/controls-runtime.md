# 控件、元素身份与运行时

修改控件、回调、滚动、弹层、输入框或动画时阅读本文。

## UI 头文件

**eui_neo.h 是 header-only 无模块接口**：0.5.6 起 `eui_neo.h` 不再包含
`eui/detail/dsl_app_impl.h`（`app::update/render` 机制挪进 `app-main` 的
`glfw_app_main.cpp` 内部编译）。`src/app.cpp` 包含完整 `<eui_neo.h>` 只取声明；
**UI 模块仍用精简头 `src/ui/eui_ui.h`**（0.5.6 已无 mangled name 冲突，历史原因
保留）。给 UI 模块加 include 时用 `"eui_ui.h"`。

## 元素身份与测量

**eui 元素 id 全局唯一且稳定**：Runtime 按解析后的 id 索引元素，同一已组合树
里的重名会互相覆盖，影响绘制、命中和缓存；Stack 不会自动给子元素加命名空间。
新增控件先检查同 frame 已有 id 和 builder 内部生成的后缀（如
`buildListPicker(id)` 会创建 `id.label`），子节点统一用根 id 的子前缀。

动态列表项用稳定业务 key：任务用 `task.id`，种子文件用 `TorrentFileView::index`，
媒体轨道用不透明的 `MediaTrackView::id`。不要用可能随排序或过滤变化的数组位置、
随机值或时间戳；同一逻辑元素跨 compose 保持 ID。回调按值捕获 key，
按同一 key 找当前数据项，不捕获 vector 偏移。

测量型组件的 `content` 回调必须用参数提供的 `eui::Ui&` 构造子项；`scrollView`
会用临时 `Ui` 测量再对真实树 compose，捕获外层 `ui` 会把控件重复塞入真实树。
**`scrollView` 会给容器加 `.clip()`，命中测试会与元素的 clip 矩形求交**，
所以全屏拦截层、溢出容器的弹层等越界子元素不能放进 `scrollView`，
否则绘制和点击一起被裁掉。方向与替代方案见下文“下拉与弹层”。
只有内容真的可能超出视口时才用 `scrollView`，固定高度弹窗用普通 `ui.stack` 绝对定位。

- `buildListPicker` 还拥有 `.field`、`.popup`、`.dismiss` 等后缀；
  `buildToggleSwitch` 拥有 `.track`、`.thumb`。创建兄弟节点前检查这些命名。
- 没有稳定 key 的数据先建立确定且唯一的身份，不生成重复 ID 或缺后端身份的可选行。
- 数字位置只有在数据源定义它为稳定身份时才可使用，例如 aria2 torrent 文件 index。

## 状态与回调

- EUI 控件交互状态使用 `ui.state<T>(id)`，不用以 ID 为 key 的静态 map。
- 受控值保存在所属页面或 store，通过控件回调更新；compose 内重建的局部变量不会持久化。
- 回调按值捕获稳定值；访问 EUI 状态时可捕获 `ui.state` 所有对象的指针，
  但回调不得超出该 UI 状态的生命周期。

## 输入框与动画

**输入框采用 MD3 Outlined（描边式）**（用户明确偏好，2026-10-11）：统一使用
`widgets::outlinedInput`，空值未聚焦时标签在框内，聚焦或有值时标签浮到顶部描边
留口；常态 1px、聚焦 2px 描边、4px 圆角、无投影，沿用黑白配色。数字步进框
共用同一外壳。浮动标签不得放进原生 input 的 clip 内；页面和弹窗需传匹配底色。

**标签滑动动画**（2026-10-15）：静置↔浮动用 eui Frame 插值（`.transition` +
`.animate(Frame|TextColor)`，`kOutlinedAnimSeconds`=0.16s），字号按姿态原生
光栅（`kOutlinedFloatLabelSize`=10px / 静置 typography.input）。**动画通道
纪律**：eui 只对「无活跃动画的子树」做 retained-layer 缓存（runtime_render
的 `subtreeAnimating` 门控），所以动画元素静止时的插值目标必须与布局帧/静态
属性恒等（Frame 方案天然满足）；text 不挂 transform（scale/translate 会走
变换绘制路径），装饰性显隐（留口 notch、占位提示）用随姿态组装/消失的硬切，
不挂常驻 opacity 过渡；只 `.animate()` 需要的通道，不要全开。

## 下拉与弹层

**下拉点击外部收起**：`buildListPicker` 展开时铺一层全屏透明拦截层（吞掉点击），
点击弹层外即收起。弹层宽度可用 `popupWidth` 参数（图标字段的弹层要加宽容纳文字）。
弹层放不进容器时会被 `scrollView` 的 clip 边界切掉最后一项（连拦截层的命中一起
被裁，点击外部失效）：放进带裁剪的容器
前先用 `pickerOpensUp(字段底边, 项数, viewport 高)`（`tinynext.ui.widgets`）算
方向，弹层高度用 `pickerPopupHeight(count)`，不要各处硬编码 22/3/3。
最稳的做法是**别把选择器放进 `scrollView`**：字段区要滚动的弹窗（添加下载
弹窗）就把弹层选择器换成无弹层的分段按钮（`segGroup` 模式，见
`downloads_page.cppm`），拦截层与弹层问题即不存在。

## 非阻塞打开

**非阻塞打开**：`openFile` / `openContainingFolder` / `openUrl` 在 Windows 走
`ShellExecuteW`（`platform.cppm::shellExecFn()`），立即返回；**不要用
`std::system("explorer …")`**——explorer 会让调用方同步等窗口关闭，卡 UI 线程。
