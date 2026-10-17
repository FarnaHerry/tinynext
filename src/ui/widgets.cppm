// ui/widgets.cppm — reusable UI controls: the generic up/down list picker,
// sidebar / rail list items, and the small icon card-action button.
module;

#include "eui_ui.h"

// 悬浮气泡/点击暂停等需要 UI 重绘一帧（纯状态变化不走 eui 内建 re-compose）。
namespace core::platform { void requestUiUpdate(); }

export module tinynext.ui.widgets;

import std;
import tinynext.ui.theme;
import tinynext.ui.utils;

namespace {
// ---- 悬浮提示（tooltip）状态与延迟逻辑 ----
// 用模块级 atomic 状态按 id 存放：eui 的 ui.state 不是线程安全的，延迟线程只碰
// 这里（cross-thread race-free）。hover 进入后延迟 hoverDelayMs 才置 shown，让
// 气泡「等一小会」再出现，避免鼠标一碰就弹。
struct TipState {
    std::atomic<bool> hovered{false};
    std::atomic<bool> shown{false};
};
std::mutex g_tipMutex;
std::unordered_map<std::string, TipState> g_tips;
struct TipWorker {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> finished;
};
std::mutex g_tipWorkersMutex;
std::vector<TipWorker> g_tipWorkers;
std::once_flag g_tipWorkerExitHook;
TipState& tipState(const std::string& id);

void joinTipWorkers() {
    std::vector<TipWorker> workers;
    {
        std::lock_guard lock(g_tipWorkersMutex);
        workers.swap(g_tipWorkers);
    }
    for (auto& worker : workers) {
        if (worker.thread.joinable()) worker.thread.join();
    }
}

void reapTipWorkers() {
    std::lock_guard lock(g_tipWorkersMutex);
    for (auto it = g_tipWorkers.begin(); it != g_tipWorkers.end();) {
        if (it->finished->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();
            it = g_tipWorkers.erase(it);
        } else {
            ++it;
        }
    }
}

void launchTipWorker(std::string id) {
    std::call_once(g_tipWorkerExitHook, [] { std::atexit(joinTipWorkers); });
    reapTipWorkers();
    auto finished = std::make_shared<std::atomic_bool>(false);
    std::thread thread([id = std::move(id), finished] {
        struct Completion {
            std::shared_ptr<std::atomic_bool> finished;
            ~Completion() { finished->store(true, std::memory_order_release); }
        } completion{finished};
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        TipState& state = tipState(id);
        if (state.hovered.load()) {
            state.shown.store(true);
            core::platform::requestUiUpdate();
        }
    });
    try {
        std::lock_guard lock(g_tipWorkersMutex);
        g_tipWorkers.push_back({std::move(thread), std::move(finished)});
    } catch (...) {
        if (thread.joinable()) thread.join();
        throw;
    }
}

TipState& tipState(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_tipMutex);
    return g_tips[id];
}

// .onHover 回调：进入/离开 hover 时更新状态并在延迟后显示气泡。h 恒为当前 hover。
void onTipHover(const std::string& id, bool h) {
    TipState& s = tipState(id);
    const bool prev = s.hovered.exchange(h);
    if (h == prev) return;
    if (!h) {
        s.shown.store(false);
        core::platform::requestUiUpdate();
        return;
    }
    // The worker owns its copied id and is joined before tooltip state is torn down.
    launchTipWorker(id);
}

// 画气泡 + 尾巴。气泡在图标栏右侧，尾巴是向左的小三角形（用 polygon，局部坐标，
// 相对 .position 的左上角），指向图标栏上的按钮。btnY/btnH 是按钮位置尺寸，
// railWidth 是图标栏宽。
void drawTipBubble(eui::Ui& ui, const std::string& id, float btnY, float btnH,
                   float railWidth, const std::string& text, const AppTheme& theme) {
    const TipState& s = tipState(id);
    if (!s.shown.load() || text.empty()) return;
    const float tipH = 20.0f;
    const float tipInnerPad = 8.0f;
    const float tailW = 7.0f;                        // 尾巴宽度（连接条）
    const float tipW = std::max(
        core::TextPrimitive::measureTextWidth(text, "", 11.0f) + tipInnerPad * 2.0f, 22.0f);
    const float tipX = railWidth + tailW + 2.0f;   // 气泡左缘
    const float tipY = btnY + (btnH - tipH) * 0.5f;  // 气泡垂直居中于按钮
    // tooltip：反色面（深色主题亮底深字 / 浅色主题深底浅字），文字随 onInverseSurface。
    const core::Color tipBg = theme.inverseSurface;
    const core::Color tipText = theme.onInverseSurface;

    // 气泡主体（绝对定位的圆角矩形）。**不加边框**：边框会在尾巴与气泡相接处切出竖线，
    // 造成「割裂」感；反色面本身对比度已足够，扁平风下也不需要投影。尾巴与气泡同色重叠
    // → 视觉上气泡直接延伸出小箭头。
    ui.stack(id + ".tip")
        .position(tipX, tipY)
        .size(tipW, tipH)
        .zIndex(60)
        .content([&] {
            ui.rect(id + ".tip.bg")
                .size(tipW, tipH)
                .color(tipBg)
                .radius(4.0f)
                .build();
            ui.text(id + ".tip.label")
                .size(tipW, tipH)
                .text(text)
                .fontSize(11.0f)
                .lineHeight(tipH)
                .color(tipText)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
        })
        .build();

    // 尾巴：向左的圆角小三角（polygon，局部坐标相对 .position 左上角）。尖端贴图标栏
    // 右缘、垂直居中于按钮；底边**伸进气泡内部 tailOverlap 深**，与气泡同色重叠盖住
    // 接缝 → 无割裂，气泡直接延伸出箭头（polygon 的 .radius 就是 eui 版的「伪元素
    // 圆角」，效果等同 CSS 气泡 ::after）。zIndex 盖在气泡主体之上。
    const float tailH = 16.0f;                              // 三角形高（垂直居中于按钮）
    const float tailOverlap = 3.0f;                         // 底边伸进气泡的深度（藏接缝）
    const float tailGap = std::max(0.0f, tipX - railWidth) + tailOverlap;
    const float tailX = railWidth;                          // 尖端贴图标栏右缘
    const float tailY = btnY + (btnH - tailH) * 0.5f;       // 垂直居中于按钮
    ui.polygon(id + ".tip.tail")
        .position(tailX, tailY)
        .size(tailGap, tailH)
        .point(0.0f, tailH * 0.5f)     // 尖端：左，垂直居中
        .point(tailGap, 0.0f)          // 底边右上（伸进气泡）
        .point(tailGap, tailH)         // 底边右下
        .radius(3.0f)                  // 圆角三个顶点
        .color(tipBg)
        .zIndex(61)
        .build();
}
} // namespace
// ----------------------------------------------------- 外层"岛"卡片背景 --
// 岛卡 = panelBg 底 + 小圆角 + hairline，无投影。
export void drawPanel(eui::Ui& ui, const std::string& id, float x, float y,
                      float w, float h, const AppTheme& theme) {
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(theme.panelBg)
        .radius(kIslandRadius)
        .border(kHairline, theme.outline)
        .build();
}

// ----------------------------------------------------- GitHub 盒式分组 --
// Primer Box：外盒 1px 边框 + 6px 圆角，顶部标题条（panelBg + 1px 底边分隔 +
// 左标题 / 右 counter 文本），其余为内容区（surface 底，由调用方画内容）。
// eui 只有整圆角，头部条用「自身 radius 6 + 底部 6px 方角补丁」拼出上圆下方。
// counter 非空时右侧渲染等宽小字（GitHub counter 徽标的文字部分）。
export float boxedGroupHeaderHeight() { return kBoxHeaderHeight; }
export void drawBoxedGroup(eui::Ui& ui, const std::string& id, float x, float y,
                           float width, float height, const std::string& title,
                           const std::string& counter, const AppTheme& theme) {
    ui.stack(id).position(x, y).size(width, height).content([&] {
        ui.rect(id + ".box")
            .size(width, height)
            .color(theme.surface)
            .radius(kIslandRadius)
            .border(kHairline, theme.outline)
            .build();
        ui.rect(id + ".header")
            .size(width, kBoxHeaderHeight)
            .color(theme.panelBg)
            .radius(kIslandRadius)
            .build();
        // 补掉头部条底部的圆角缺口（让底边成直线，圆只留在盒子的上两角）。
        ui.rect(id + ".header.patch")
            .position(0, kBoxHeaderHeight - kIslandRadius)
            .size(width, kIslandRadius)
            .color(theme.panelBg)
            .build();
        ui.rect(id + ".header.divider")
            .position(0, kBoxHeaderHeight - kHairline)
            .size(width, kHairline)
            .color(theme.outline)
            .build();
        components::text(ui, id + ".header.title")
            .position(kOutlinedInset, 0)
            .size(width - kOutlinedInset * 2.0f - 60.0f, kBoxHeaderHeight)
            .text(title)
            .fontSize(12.0f)
            .color(theme.onSurface)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
        if (!counter.empty()) {
            ui.text(id + ".header.counter")
                .position(width - 60.0f - kOutlinedInset, 0)
                .size(60.0f, kBoxHeaderHeight)
                .text(counter)
                .fontSize(11.0f)
                .fontFamily(kMonoFont)
                .color(theme.onSurfaceVariant)
                .horizontalAlign(core::HorizontalAlign::Right)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
        }
    }).build();
}

// ----------------------------------------------------- GitHub 下划线标签页 --
// TabNav：每行底部 1px 通栏线，标签项 = 文本 + 底部指示条；选中 = onSurface
// 文字 + 2px 主色下划线，未选中 = meta 色。点击回调 onPick(index)。
// 标签宽度按文字实测 + 左右 12px 内距，放不下自动换行（每行都有自己的底部
// 通栏线）。返回总高度（行数 × (tabHeight + 1px)），供调用方排布下方内容。
export float buildUnderlineTabs(eui::Ui& ui, const std::string& id, float x, float y,
                                float width, float tabHeight, const char* const* labels,
                                int count, int selected, const AppTheme& theme,
                                const std::function<void(int)>& onPick,
                                float fontSize = 12.0f) {
    constexpr float kTabPad = 12.0f;
    constexpr float kRowH = 1.0f;  // 每行底部的通栏线高
    std::vector<float> tabW(static_cast<std::size_t>(count));
    std::vector<int> rowOf(static_cast<std::size_t>(count));
    int rows = 1;
    float cursor = 0.0f;
    for (int i = 0; i < count; ++i) {
        tabW[static_cast<std::size_t>(i)] =
            core::TextPrimitive::measureTextWidth(labels[i], "", fontSize) + 2.0f * kTabPad;
        if (cursor > 0.0f && cursor + tabW[static_cast<std::size_t>(i)] > width) {
            ++rows;
            cursor = 0.0f;
        }
        rowOf[static_cast<std::size_t>(i)] = rows - 1;
        cursor += tabW[static_cast<std::size_t>(i)];
    }
    const float rowFull = tabHeight + kRowH;
    const float totalH = static_cast<float>(rows) * rowFull;
    ui.stack(id).position(x, y).size(width, totalH).content([&] {
        std::vector<float> cursors(static_cast<std::size_t>(rows), 0.0f);
        for (int i = 0; i < count; ++i) {
            const int r = rowOf[static_cast<std::size_t>(i)];
            const bool active = i == selected;
            const float tabW0 = tabW[static_cast<std::size_t>(i)];
            const float rowY = static_cast<float>(r) * rowFull;
            const float cx = cursors[static_cast<std::size_t>(r)];
            const std::string tabId = id + ".tab." + std::to_string(i);
            ui.rect(tabId + ".hit")
                .position(cx, rowY)
                .size(tabW0, tabHeight)
                .states({0.0f, 0.0f, 0.0f, 0.0f},
                        stateLayer(theme.onSurface, 0.08f),
                        stateLayer(theme.onSurface, 0.12f))
                .onClick([i, onPick] { onPick(i); })
                .build();
            components::text(ui, tabId + ".label")
                .position(cx, rowY)
                .size(tabW0, tabHeight)
                .text(labels[i])
                .fontSize(fontSize)
                .color(active ? theme.onSurface : theme.onSurfaceVariant)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
            if (active) {
                ui.rect(tabId + ".underline")
                    .position(cx, rowY + tabHeight - 2.0f)
                    .size(tabW0, 2.0f)
                    .color(theme.primary)
                    .radius(1.0f)
                    .build();
            }
            // 每行行底 1px 通栏线（GitHub TabNav 的 border-bottom）。
            ui.rect(tabId + ".rail")
                .position(0, rowY + tabHeight)
                .size(width, kRowH)
                .color(theme.outline)
                .build();
            cursors[static_cast<std::size_t>(r)] = cx + tabW0;
        }
    }).build();
    return totalH;
}


// 按钮文字/图标色：primary 按钮文字 = onPrimary（反白主色的反色：暗主题黑字 /
// 亮主题白字）；非 primary 走组件默认（text = onSurface）。
export core::Color onPrimaryColor(const AppTheme& theme, bool primary = true) {
    return primary ? theme.onPrimary : theme.components.text;
}

// 原生 input 只负责编辑、选择、IME 与滚动；Outlined 外壳统一负责视觉。
components::InputStyle bareInputStyle(const AppTheme& theme) {
    components::InputStyle style(theme.components);
    const core::Color transparent{0, 0, 0, 0};
    style.background = transparent;
    style.focused = transparent;
    style.border = transparent;
    style.focusBorder = transparent;
    style.radius = 0;
    style.shadow = core::Shadow{};
    return style;
}

// frame/label 是无事件装饰，不参与输入命中；顶部留口使用所在页面/弹窗底色。
void drawOutlinedFrame(eui::Ui& ui, const std::string& id, float width, float height,
                       const AppTheme& theme, core::Color background, bool focused) {
    const float top = kOutlinedLabelHeight * 0.5f;
    // 聚焦反馈不只是变色：hairline↔2px 主色描边用过渡插值，与标签滑动同节奏。
    // 通道只开 Border（纪律见 drawOutlinedLabel 注释）。
    core::Transition anim = core::Transition::make(kOutlinedAnimSeconds, core::Ease::OutCubic);
    anim.properties = core::AnimProperty::Border;
    ui.rect(id + ".outline")
        .position(0, top).size(width, height - top)
        .color(background).radius(kOutlinedRadius)
        .border(focused ? kOutlinedFocusBorder : kHairline,
                focused ? theme.primary : theme.outline)
        .transition(anim)
        .build();
}

// 标签两姿态：静置 = 框内正文垂直居中（原生字号）；浮动 = 顶部留口内的小字。
// 动画用 eui 的 Frame 插值（.animate 显式开启）：布局帧在两姿态间插值，标签
// 平滑滑上/滑回；静止时 Frame 目标与布局帧恒等，不产生常驻动画状态，空闲
// CPU 与无动画版本持平（弹窗各状态实测一致）。字号按姿态原生光栅（静置
// 11px / 浮动 10px，各自最锐利），切换瞬间的字号跳变由滑动运动掩护；
// TextColor 同参数过渡（聚焦变主色）。rect 上不要挂 transition+opacity 做
// 留口淡入淡出：留口改用硬切、与标签滑动同帧发生，0.16s 内不可辨。
// pinFloating：永不静置的控件（数字步进框）直接按浮动姿态布局。
void drawOutlinedLabel(eui::Ui& ui, const std::string& id, float width, float height,
                       const std::string& label, const AppTheme& theme,
                       core::Color background, bool focused, bool floating,
                       bool pinFloating = false) {
    if (label.empty()) return;
    const float available = std::max(0.0f, width - 2.0f * kOutlinedInset);
    const float fontSize = (pinFloating || floating)
        ? kOutlinedFloatLabelSize : theme.components.metrics.typography.input;
    const float lineHeight = (pinFloating || floating)
        ? kOutlinedLabelHeight : fontSize * 1.2f;
    const std::string text = ellipsizeText(label, available, fontSize);
    const float textWidth = core::TextPrimitive::measureTextWidth(text, "", fontSize);
    const float restY = kOutlinedLabelHeight * 0.5f
        + (height - kOutlinedLabelHeight * 0.5f - lineHeight) * 0.5f;
    const float y = (pinFloating || floating) ? 0.0f : restY;

    // 顶部留口：压掉描边的背景色块，只在浮动态组装（与标签滑动同帧出现/消失）。
    if (pinFloating || floating) {
        ui.rect(id + ".notch")
            .position(kOutlinedInset - 4.0f, 0)
            .size(textWidth + 8.0f, kOutlinedLabelHeight)
            .color(background).build();
    }

    core::Transition anim = core::Transition::make(kOutlinedAnimSeconds, core::Ease::OutCubic);
    anim.properties = core::AnimProperty::Frame | core::AnimProperty::TextColor;
    components::text(ui, id + ".label")
        .position(kOutlinedInset, y)
        .size(available, lineHeight).text(text)
        .fontSize(fontSize).lineHeight(lineHeight)
        .transition(anim)
        .animate(core::AnimProperty::Frame | core::AnimProperty::TextColor)
        .color(focused ? theme.primary : theme.onSurfaceVariant).build();
}

export class OutlinedInputBuilder {
public:
    OutlinedInputBuilder(eui::Ui& ui, std::string id) : ui_(ui), id_(std::move(id)) {}
    OutlinedInputBuilder& position(float x, float y) { x_ = x; y_ = y; return *this; }
    OutlinedInputBuilder& size(float width, float height) { width_ = width; height_ = height; return *this; }
    OutlinedInputBuilder& label(std::string value) { label_ = std::move(value); return *this; }
    OutlinedInputBuilder& value(std::string value) { value_ = std::move(value); return *this; }
    OutlinedInputBuilder& placeholder(std::string value) { placeholder_ = std::move(value); return *this; }
    OutlinedInputBuilder& multiline(bool value = true) { multiline_ = value; return *this; }
    OutlinedInputBuilder& scrollbar(bool value = true) { scrollbar_ = value; return *this; }
    OutlinedInputBuilder& fontFamily(std::string value) { fontFamily_ = std::move(value); return *this; }
    OutlinedInputBuilder& theme(const AppTheme& value) { theme_ = &value; background_ = value.surface; return *this; }
    OutlinedInputBuilder& background(core::Color value) { background_ = value; return *this; }
    OutlinedInputBuilder& onChange(std::function<void(const std::string&)> value) { onChange_ = std::move(value); return *this; }
    OutlinedInputBuilder& onEnter(std::function<void()> value) { onEnter_ = std::move(value); return *this; }
    void build() {
        const bool focused = ui_.isFocused(id_ + ".editor.hit");
        const bool floating = focused || !value_.empty();
        ui_.stack(id_).position(x_, y_).size(width_, height_).content([&] {
            drawOutlinedFrame(ui_, id_, width_, height_, *theme_, background_, focused);
            // 原生 input 的文本 dirtyKey 不含 placeholder，动态切换占位提示会复用
            // 空文本缓存。提示在这里独立绘制，仍让原生编辑器负责光标和 IME。
            // 只在可见时组装：text 上挂常驻 opacity 过渡同样有 retained-layer 代价。
            const bool hintVisible = (floating || label_.empty()) && value_.empty();
            if (!placeholder_.empty() && hintVisible) {
                const float top = kOutlinedLabelHeight * 0.5f;
                const float bodyHeight = height_ - top;
                const float fontSize = theme_->components.metrics.typography.input;
                const float lineHeight = fontSize * 1.2f;
                const float gutter = multiline_ && scrollbar_
                    ? theme_->components.metrics.control.scrollbar + 4.0f : 0.0f;
                const float textWidth = std::max(0.0f, width_ - 2 * kOutlinedInset - gutter);
                ui_.text(id_ + ".hint")
                    .position(kOutlinedInset, top + (multiline_ ? kOutlinedInset : (bodyHeight - lineHeight) * 0.5f))
                    .size(textWidth, multiline_ ? bodyHeight - 2 * kOutlinedInset : lineHeight)
                    .text(multiline_ ? placeholder_ : ellipsizeText(placeholder_, textWidth, fontSize, fontFamily_))
                    .fontFamily(fontFamily_).fontSize(fontSize).lineHeight(lineHeight)
                    .maxWidth(textWidth).wrap(multiline_).clip()
                    .color(components::theme::withOpacity(theme_->onSurface, 0.45f)).build();
            }
            components::input(ui_, id_ + ".editor")
                .position(0, kOutlinedLabelHeight * 0.5f)
                .size(width_, height_ - kOutlinedLabelHeight * 0.5f)
                .value(value_).placeholder("")
                .multiline(multiline_).scrollbar(scrollbar_).fontFamily(fontFamily_)
                .inset(kOutlinedInset).theme(theme_->components).style(bareInputStyle(*theme_))
                .onChange(onChange_).onEnter(onEnter_).build();
            drawOutlinedLabel(ui_, id_, width_, height_, label_, *theme_, background_, focused, floating);
        }).build();
    }
private:
    eui::Ui& ui_;
    std::string id_, label_, value_, placeholder_, fontFamily_;
    float x_ = 0, y_ = 0, width_ = 160, height_ = kOutlinedFieldHeight;
    bool multiline_ = false, scrollbar_ = true;
    const AppTheme* theme_ = &currentTheme();
    core::Color background_ = currentTheme().surface;
    std::function<void(const std::string&)> onChange_;
    std::function<void()> onEnter_;
};

export OutlinedInputBuilder outlinedInput(eui::Ui& ui, std::string id) {
    return OutlinedInputBuilder(ui, std::move(id));
}

// 单岛布局里的竖向分隔线：同一张岛卡内，把二级侧边栏与内容区区分开（整页一张岛、
// 不再各自成卡，用竖线分隔）。细 1px 竖线，从岛卡顶边划到底边（仅留 2px 不压到
// 上下边框线），让两侧有明确的纵向分隔感；线在侧边栏/内容交界处、远离圆角，无需内缩。
export void drawVDivider(eui::Ui& ui, const std::string& id, float x, float y,
                         float h, const AppTheme& theme) {
    constexpr float kInset = 2.0f;
    ui.rect(id)
        .position(x, y + kInset)
        .size(1.0f, std::max(0.0f, h - 2.0f * kInset))
        .color(components::theme::withOpacity(theme.components.border, 0.55f))
        .build();
}

// 工具栏图标按钮：小圆角方钮，无描边；standard 款 hover/pressed 叠 onSurface
// state layer（8%/12%），primary 款反白主色填充 + onPrimary 图标（扁平，无投影）。
export void drawToolbarIconButton(eui::Ui& ui, const std::string& id, float x, float y,
                                  float w, float h, unsigned int icon, bool primary,
                                  const AppTheme& theme, std::function<void()> onClick,
                                  bool fab = false, bool enabled = true) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    const float radius = kButtonRadius;

    if (primary) {
        components::button(ui, id)
            .position(x, y)
            .size(w, h)
            .icon(icon)
            .text("")
            .iconSize(kToolbarIconSize)
            .theme(tokens, true)
            .iconColor(onPrimaryColor(theme))  // 反白主色底 → onPrimary 图标
            .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .radius(radius)
            .onClick(std::move(onClick))
            .build();
        return;
    }

    // standard 款：GitHub 默认钮 —— surfaceContainerHigh 底 + 1px 边框 +
    // 6px 圆角，hover/pressed 变 Highest（亮色变深 / 暗色变浅，同 Primer）。
    ui.rect(id + ".fill")
        .position(x, y)
        .size(w, h)
        .states(enabled ? theme.surfaceContainerHigh : transparent,
                enabled ? theme.surfaceContainerHighest : transparent,
                enabled ? theme.surfaceContainerHighest : transparent)
        .radius(radius)
        .border(enabled ? kHairline : 0.0f, theme.outline)
        .transition(transition)
        .onClick([enabled, action = std::move(onClick)] {
            if (enabled) action();
        })
        .build();

    ui.text(id + ".icon")
        .position(x, y)
        .size(w, h)
        .icon(icon)
        .fontSize(13.0f)
        .lineHeight(h)
        .color(enabled ? tokens.text : theme.metaText)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// ----------------------------------------------------- 通用上下拉列表选择器 --
//
// eui 的 components::dropdown 只会向下弹出，放在底部翻页行时弹层会超出窗口下缘。
// 这里做一个通用选择器：字段（文字显示当前项，或纯图标 fa-sort）+ 向上/向下
// 展开的 popup，样式取自当前主题 tokens。分页大小（向上）与排序（向下）共用。
// 少量互斥选项直接展示，同一选择样式用于页面筛选和设置字段。
// 没有溢出弹层，放在滚动表单内也不依赖裁剪边界或全屏拦截层。
export void buildChoiceButtons(eui::Ui& ui, const std::string& id, float height,
                               const AppTheme& theme, const char* const* labels,
                               int count, int selected,
                               const std::function<void(int)>& onPick,
                               float fontSize = 11.0f,
                               float x = 0.0f, float y = 0.0f) {
    // GitHub 分段组：外盒 1px 边框 + 6px 圆角，段间 1px 竖分隔，选中段
    // surfaceContainerHighest 填充（内缩 1px 露出边框），未选中文字 meta 色。
    std::vector<float> widths;
    float totalWidth = 0.0f;
    for (int i = 0; i < count; ++i) {
        widths.push_back(core::TextPrimitive::measureTextWidth(labels[i], "", fontSize) + 20.0f);
        totalWidth += widths.back();
    }
    ui.stack(id)
        .position(x, y)
        .size(totalWidth, height)
        .content([&] {
            ui.rect(id + ".box")
                .size(totalWidth, height)
                .radius(kButtonRadius)
                .border(kHairline, theme.outline)
                .build();
            float cursor = 0.0f;
            for (int i = 0; i < count; ++i) {
                const bool active = i == selected;
                const std::string segId = id + ".seg." + std::to_string(i);
                if (active) {
                    ui.rect(segId + ".fill")
                        .position(cursor + kHairline, kHairline)
                        .size(widths[i] - 2.0f * kHairline, height - 2.0f * kHairline)
                        .color(theme.surfaceContainerHighest)
                        .radius(kButtonRadius - 1.0f)
                        .build();
                }
                if (i > 0) {
                    ui.rect(segId + ".divider")
                        .position(cursor, 4.0f)
                        .size(kHairline, height - 8.0f)
                        .color(theme.outlineVariant)
                        .build();
                }
                ui.rect(segId + ".hit")
                    .position(cursor, 0)
                    .size(widths[i], height)
                    .states({0.0f, 0.0f, 0.0f, 0.0f},
                            stateLayer(theme.onSurface, 0.08f),
                            stateLayer(theme.onSurface, 0.12f))
                    .onClick([i, onPick] { onPick(i); })
                    .build();
                ui.text(segId + ".label")
                    .position(cursor, 0)
                    .size(widths[i], height)
                    .text(labels[i])
                    .fontSize(fontSize)
                    .color(active ? theme.onSurface : theme.onSurfaceVariant)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
                cursor += widths[i];
            }
        })
        .build();
}

export enum class PickerField { Text, Icon, Plain };

// ---- 选择器弹层度量（设计逻辑像素）----
// 弹层从字段下方（opensUp 时上方）展开，高度只由项数决定。调用方把选择器放进
// 带 clip 的容器（scrollView / 卡片）时，要用 pickerOpensUp 先判断方向：否则
// 向下展开的弹层会被裁剪边界切掉最后一项。
export constexpr float kPickerItemHeight = 22.0f;
export constexpr float kPickerPopupPad = 3.0f;
export constexpr float kPickerPopupGap = 3.0f;
export constexpr float pickerPopupHeight(int count) {
    return kPickerItemHeight * static_cast<float>(count) + 2.0f * kPickerPopupPad;
}
// fieldBottomY：字段底边在容器坐标系里的 y；viewportHeight：可用高度。
export bool pickerOpensUp(float fieldBottomY, int count, float viewportHeight) {
    return fieldBottomY + kPickerPopupGap + pickerPopupHeight(count) > viewportHeight;
}

export void buildListPicker(eui::Ui& ui, const std::string& id, float width, float height,
                            const AppTheme& theme, bool& open, const char* const* labels,
                            int count, int selected, bool opensUp, PickerField field,
                            const std::function<void(int)>& onPick,
                            float popupWidth = 0.0f) {
    const float itemHeight = kPickerItemHeight;
    const float popupPad = kPickerPopupPad;
    const float popupGap = kPickerPopupGap;
    // 弹层宽度：默认与字段同宽；字段是纯图标（如排序）时可传入更宽的值容纳文字。
    const float popWidth = popupWidth > 0.0f ? popupWidth : width;
    const float popupHeight = pickerPopupHeight(count);
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);

    ui.stack(id)
        .size(width, height)
        .zIndex(30)
        .content([&] {
            // ---- 字段（点击切换展开/收起）：文字显示当前项，或纯图标 ----
            if (field == PickerField::Icon) {
                // 图标字段（如排序）：默认无描边，hover 才浮现。
                drawToolbarIconButton(ui, id + ".btn", 0, 0, width, height,
                                      0xF0DC, false, theme,
                                      [&open] { open = !open; });
            } else if (field == PickerField::Plain) {
                // 纯文本字段（翻页器用）：无边框条，当前项文字居中 + 右侧小箭头（无尾 chevron），
                // 点击弹出列表。M3 text-button 风：透明底 + onSurface state layer。
                ui.rect(id + ".hit")
                    .size(width, height)
                    .states({0.0f, 0.0f, 0.0f, 0.0f}, stateLayer(theme.onSurface, 0.08f),
                            stateLayer(theme.onSurface, 0.12f))
                    .radius(kChipRadius)
                    .onClick([&open] { open = !open; })
                    .build();
                ui.text(id + ".label")
                    .x(-4.0f)  // 给右侧箭头让位，视觉上仍居中
                    .size(width - 10.0f, height)
                    .text(labels[selected])
                    .fontSize(11.0f)
                    .lineHeight(height)
                    .color(tokens.text)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
                ui.text(id + ".chevron")
                    .x(width - 15.0f)
                    .size(12.0f, height)
                    .icon(open ? 0xF077 : 0xF078)  // chevron-up / chevron-down
                    .fontSize(9.0f)
                    .lineHeight(height)
                    .color(tokens.primary)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
            } else {
                // outlined 字段：surface 底 + hairline 描边，展开时描边转 onSurface。
                ui.rect(id + ".field")
                    .size(width, height)
                    .color(tokens.surface)
                    .radius(kChipRadius)
                    .border(kHairline, open ? theme.onSurface : theme.outline)
                    .transition(transition)
                    .onClick([&open] { open = !open; })
                    .build();

                ui.text(id + ".label")
                    .x(9.0f)
                    .size(width - 30.0f, height)
                    .text(labels[selected])
                    .fontSize(11.0f)
                    .lineHeight(height)
                    .color(tokens.text)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();

                ui.text(id + ".chevron")
                    .x(width - 20.0f)
                    .size(14.0f, height)
                    .icon(open ? 0xF077 : 0xF078)  // chevron-up / chevron-down
                    .fontSize(10.0f)
                    .lineHeight(height)
                    .color(tokens.primary)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
            }

            // ---- 弹出列表（向上或向下展开）----
            if (open) {
                // 全屏透明拦截层（在弹层之下）：点击弹层外任意处收起，并吞掉点击
                // 防止穿透到弹窗遮罩/其他控件。尺寸放大覆盖任意窗口。
                ui.rect(id + ".dismiss")
                    .position(-2000.0f, -2000.0f)
                    .size(5000.0f, 5000.0f)
                    .states({0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
                            {0.0f, 0.0f, 0.0f, 0.0f})
                    .onClick([&open] { open = false; })
                    .onScroll([](const core::ScrollEvent&) {})
                    .build();

                ui.stack(id + ".popup")
                    .x(popWidth > width ? width - popWidth : 0.0f)  // 比字段宽时向右边缘对齐
                    .y(opensUp ? -(popupHeight + popupGap) : height + popupGap)
                    .size(popWidth, popupHeight)
                    .zIndex(31)
                    .content([&] {
                        // 弹层：不透明 surfaceContainerHigh + hairline 描边，扁平无投影。
                        ui.rect(id + ".popup.bg")
                            .size(popWidth, popupHeight)
                            .color(theme.surfaceContainerHigh)
                            .radius(kChipRadius)
                            .border(kHairline, theme.outline)
                            .onClick([] {})  // 吞掉弹层内部空白点击，避免穿透到遮罩关闭弹窗
                            .build();

                        for (int i = 0; i < count; ++i) {
                            const float itemY = popupPad + i * itemHeight;
                            const bool itemSelected = i == selected;
                            ui.rect(id + ".item." + std::to_string(i))
                                .x(popupPad)
                                .y(itemY)
                                .size(popWidth - popupPad * 2.0f, itemHeight)
                                .states(itemSelected ? theme.surfaceContainerHighest
                                                     : core::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                        stateLayer(theme.onSurface, 0.08f),
                                        stateLayer(theme.onSurface, 0.12f))
                                .radius(kChipRadius)
                                .onClick([&open, i, onPick] {
                                    open = false;
                                    onPick(i);
                                })
                                .build();

                            ui.text(id + ".item.label." + std::to_string(i))
                                .x(popupPad + 8.0f)
                                .y(itemY)
                                .size(popWidth - popupPad * 2.0f - 16.0f, itemHeight)
                                .text(labels[i])
                                .fontSize(11.0f)
                                .lineHeight(itemHeight)
                                .color(itemSelected ? theme.onSurface : tokens.text)
                                .verticalAlign(core::VerticalAlign::Center)
                                .build();
                        }
                    })
                    .build();
            }
        })
        .build();
}

// 数字步进输入：[-] [输入] [+] 共用 Outlined 外壳与顶部标签。
// value 是当前文本（可手输数字；空/非法按 0），加减基于解析出的整数，夹到
// [min,max]、步长 step，改完写回并回调。输入框聚焦时外壳描边转主色。
export void buildNumberStepper(eui::Ui& ui, const std::string& id, float x, float y,
                               float width, float height, const AppTheme& theme,
                               const std::string& value,
                               const std::function<void(const std::string&)>& onChange,
                               int min, int max, int step, std::string label = "",
                               std::optional<core::Color> background = std::nullopt) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    const float radius = kOutlinedRadius;
    const float top = kOutlinedLabelHeight * 0.5f;
    const float bodyHeight = height - top;
    const float btnW = std::min(28.0f, bodyHeight);
    const float inputW = std::max(0.0f, width - btnW * 2.0f);

    // 一体外壳（聚焦时描边转主色，与独立输入框的聚焦反馈一致）。
    const bool focused = ui.isFocused(id + ".input.hit");
    ui.stack(id + ".frame").position(x, y).size(width, height).content([&] {
        drawOutlinedFrame(ui, id, width, height, theme, background.value_or(theme.surface), focused);
        // 步进框标签永不静置：按浮动姿态原生光栅布局（pinFloating），不参与缩放。
        drawOutlinedLabel(ui, id, width, height, label, theme,
                          background.value_or(theme.surface), focused, true, true);
    }).build();
    // 文本在 ± 之间的中段内水平居中：inset = (中段宽 - 文本宽)/2（输入框只有
    // 左对齐 + inset，用真实字体度量动态算 inset；每帧重算，编辑时自跟随）。
    const float inputFontSize = tokens.metrics.typography.input;
    const float textW = core::TextPrimitive::measureTextWidth(value, "", inputFontSize);
    const float inputInset = std::max(2.0f, (inputW - textW) * 0.5f);
    components::input(ui, id + ".input")
        .position(x + btnW, y + top)
        .size(inputW, bodyHeight)
        .inset(inputInset)
        .value(value)
        .fontFamily("")  // 用应用字体（Noto Sans SC），不要 eui 默认的 Microsoft YaHei
        .theme(tokens)
        .style(bareInputStyle(theme))
        .onChange([onChange](const std::string& v) {
            // 只保留数字：手输字母/符号会被滤掉（eui input 每帧用 value() 覆盖
            // 内部文本，写回纯数字状态后显示即同步）。范围校验在保存层。
            std::string digits;
            for (char c : v) {
                if (c >= '0' && c <= '9') digits += c;
            }
            onChange(digits);
        })
        .build();

    // ± 按钮：壳内透明命中区 + hover/pressed state layer，图标居中。
    const auto drawStepButton = [&](const std::string& btnId, float btnX,
                                    unsigned int icon, int delta) {
        ui.rect(btnId + ".hit")
            .position(btnX, y + top)
            .size(btnW, bodyHeight)
            .states(transparent, stateLayer(theme.onSurface, 0.08f),
                    stateLayer(theme.onSurface, 0.12f))
            .radius(radius)
            .transition(transition)
            .onClick([value, onChange, min, max, step, delta] {
                int cur = 0;
                try { cur = std::stoi(trimText(value)); } catch (...) {}
                onChange(std::to_string(std::clamp(cur + delta * step, min, max)));
            })
            .build();
        ui.text(btnId + ".icon")
            .position(btnX, y + top)
            .size(btnW, bodyHeight)
            .icon(icon)
            .fontSize(kStepperIconSize)
            .lineHeight(bodyHeight)
            .color(tokens.text)
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    };
    drawStepButton(id + ".minus", x, 0xF068, -1);               // fa-minus
    drawStepButton(id + ".plus", x + btnW + inputW, 0xF067, 1); // fa-plus
}

// 侧边栏列表项：图标 + 文字，激活指示为全宽灰阶圆角块（surfaceContainerHigh，
// 无左侧竖条），激活图标/文字 = onSurface；非激活 = onSurfaceVariant，hover 叠
// onSurface state layer（激活项不叠）。count >= 0 时在右侧显示数量徽标。
export void drawSidebarItem(eui::Ui& ui, const std::string& id, float x, float y,
                            float width, float height, const std::string& label,
                            unsigned int icon, bool active, const AppTheme& theme,
                            std::function<void()> onClick, int count = -1) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color idle = {0.0f, 0.0f, 0.0f, 0.0f};
    const core::Color activeFill = theme.surfaceContainerHigh;
    const core::Color textColor =
        active ? theme.onSurface : theme.onSurfaceVariant;

    // 激活指示 pill（始终存在，透明即隐藏，避免创建/移除图层）。
    ui.rect(id + ".bg")
        .position(x, y)
        .size(width, height)
        .color(active ? activeFill : idle)
        .radius(kButtonRadius)
        .transition(transition)
        .build();

    // 点击命中区（悬停反馈；激活项不再叠加 hover 底色）。
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, height)
        .states(idle, active ? idle : stateLayer(theme.onSurface, 0.08f),
                stateLayer(theme.onSurface, 0.12f))
        .radius(kButtonRadius)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    // 图标。
    ui.text(id + ".icon")
        .position(x + 8.0f, y)
        .size(16.0f, height)
        .icon(icon)
        .fontSize(11.0f)
        .lineHeight(height)
        .color(textColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 文字（有数量徽标时让出右侧空间，宽度与下方气泡一致 + 间隙）。
    const float countW = count >= 0
        ? 12.0f + static_cast<float>(std::to_string(count).size()) * 6.0f + 10.0f
        : 0.0f;
    ui.text(id + ".label")
        .position(x + 30.0f, y)
        .size(width - 36.0f - countW, height)
        .text(label)
        .fontSize(12.0f)
        .lineHeight(height)
        .color(textColor)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 右侧数量徽标：小气泡（圆角底 + 等宽数字），宽度随位数自适应；统一
    // surfaceContainerHighest 底，激活项数字用 onSurface、其余 onSurfaceVariant。
    if (count >= 0) {
        const std::string text = std::to_string(count);
        const float bubbleW = 12.0f + static_cast<float>(text.size()) * 6.0f;
        const float bubbleH = 14.0f;
        const float bubbleX = x + width - bubbleW - 6.0f;
        const float bubbleY = y + (height - bubbleH) * 0.5f;
        ui.rect(id + ".count.bg")
            .position(bubbleX, bubbleY)
            .size(bubbleW, bubbleH)
            .color(theme.surfaceContainerHighest)
            .radius(bubbleH * 0.5f)
            .build();
        ui.text(id + ".count")
            .position(bubbleX, bubbleY)
            .size(bubbleW, bubbleH)
            .text(text)
            .fontSize(10.0f)
            .fontFamily(kMonoFont)
            .lineHeight(bubbleH)
            .color(active ? theme.onSurface : theme.onSurfaceVariant)
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }
}

// 图标导航栏项：32×32 激活指示块（图标居中，radius 6），无文字标签——名称经
// hover 延迟气泡提示（tooltip 参数，出现在图标栏右侧，带尾巴指向按钮）。
// 激活 = surfaceContainerHigh 底 + onSurface 图标；非激活 = onSurfaceVariant；
// hover 叠 onSurface state layer。
export void drawRailItem(eui::Ui& ui, const std::string& id, float y, float railWidth,
                         unsigned int icon, const std::string& tooltip, bool active,
                         const AppTheme& theme, std::function<void()> onClick) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color idle = {0.0f, 0.0f, 0.0f, 0.0f};
    const core::Color iconColor =
        active ? theme.onSurface : theme.onSurfaceVariant;
    const float pillH = 32.0f;
    const float pillW = pillH;
    const float pillX = (railWidth - pillW) * 0.5f;

    // 激活指示块。
    ui.rect(id + ".bg")
        .position(pillX, y)
        .size(pillW, pillH)
        .color(active ? theme.surfaceContainerHigh : idle)
        .radius(kButtonRadius)
        .transition(transition)
        .build();

    // 点击命中区 + 悬停状态（有 tooltip 时，hover 走延迟显示逻辑）。
    ui.rect(id + ".hit")
        .position(pillX, y)
        .size(pillW, pillH)
        .states(idle, active ? idle : stateLayer(theme.onSurface, 0.08f),
                stateLayer(theme.onSurface, 0.12f))
        .radius(kButtonRadius)
        .transition(transition)
        .onHover([id](bool h) { onTipHover(id, h); })
        .onClick(std::move(onClick))
        .build();

    // 图标（pill 内水平居中）。
    ui.text(id + ".icon")
        .position(pillX, y)
        .size(pillW, pillH)
        .icon(icon)
        .fontSize(16.0f)
        .lineHeight(pillH)
        .color(iconColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 悬浮提示气泡（带尾巴，延迟显示）。
    if (!tooltip.empty()) {
        drawTipBubble(ui, id, y, pillH, railWidth, tooltip, theme);
    }
}

// 文本按钮：透明底小圆角，文字 primary（error=true 时 error 色），
// hover/pressed 叠对应色 state layer。用于弹窗/设置页的次级操作与破坏性操作。
export void drawTextButton(eui::Ui& ui, const std::string& id, float x, float y,
                           float w, float h, const std::string& label,
                           const AppTheme& theme, std::function<void()> onClick,
                           bool error = false) {
    const core::Color c = error ? theme.error : theme.primary;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .states(transparent, stateLayer(c, 0.08f), stateLayer(c, 0.12f))
        .radius(kButtonRadius)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    ui.text(id + ".label")
        .position(x, y)
        .size(w, h)
        .text(label)
        .fontSize(kButtonFontSize)
        .lineHeight(h)
        .color(c)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// 卡片内的小图标操作按钮：小圆角方圆透明底，hover/pressed 叠 onSurface
// state layer；primary 动作图标用反白主色，其余 onSurfaceVariant。无投影无描边。
export void drawCardAction(eui::Ui& ui, const std::string& id, float x, float y,
                           unsigned int icon, bool primary, const AppTheme& theme,
                           std::function<void()> onClick) {
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    ui.rect(id)
        .position(x, y)
        .size(kCardActionSize, kCardActionSize)
        .states(transparent, stateLayer(theme.onSurface, 0.08f),
                stateLayer(theme.onSurface, 0.12f))
        .radius(kButtonRadius)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    ui.text(id + ".icon")
        .position(x, y)
        .size(kCardActionSize, kCardActionSize)
        .icon(icon)
        .fontSize(kCardActionIconSize + 1.0f)
        .lineHeight(kCardActionSize)
        .color(primary ? theme.primary : theme.onSurfaceVariant)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// 滑动开关：轨道 pill + 圆形滑块。开启 = 轨道反白主色 + 滑块 onPrimary；
// 关闭 = 轨道 surfaceContainerHighest + outline 描边 + 滑块 outline。点击切换。
export void buildToggleSwitch(eui::Ui& ui, const std::string& id, float x, float y,
                              float trackW, float trackH, bool on,
                              const AppTheme& theme, std::function<void(bool)> onToggle) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.16f, core::Ease::OutCubic);
    const float thumbR = trackH * 0.5f - 2.0f;   // 滑块半径（轨道内缩 2px）
    const float thumbX = on ? trackW - trackH + thumbR : thumbR;  // 滑块圆心 x
    const float thumbY = trackH * 0.5f;

    // 轨道：开启 primary 填充（无描边）、关闭 surfaceContainerHighest + outline 描边。
    ui.rect(id + ".track")
        .position(x, y)
        .size(trackW, trackH)
        .color(on ? tokens.primary : theme.surfaceContainerHighest)
        .radius(trackH * 0.5f)
        .border(1.0f, on ? core::Color{0.0f, 0.0f, 0.0f, 0.0f} : theme.outline)
        .transition(transition)
        .onClick([on, onToggle] { onToggle(!on); })
        .build();

    // 滑块：开启 = onPrimary（主色底上的反色），关闭 = outline。
    ui.rect(id + ".thumb")
        .position(x + thumbX - thumbR, y + thumbY - thumbR)
        .size(thumbR * 2.0f, thumbR * 2.0f)
        .color(on ? theme.onPrimary : theme.outline)
        .radius(thumbR)
        .build();
}
