// i18n.cppm — UI 多语言（中文 / 英语）。轻量方案：中文作为默认文案兼「键」，
// 每处用 tr(中文, English) 包裹；g_lang 决定取哪一侧。立即模式 UI 每帧重读
// g_lang，切换语言即刻生效（无需重启）。
//
// 无 eui 依赖：GUI / headless / store 消息都可 import 本模块。
export module tinynext.i18n;

import std;
import tinynext.config;

// 当前语言（模块级导出变量在 importers 间共享同一实体）。启动时读配置，
// 运行时经 setLanguage() 切换。
export cfg::Lang g_lang = cfg::lang();

// 切换语言：立即生效 + 持久化。
export void setLanguage(cfg::Lang l) {
    g_lang = l;
    cfg::setLang(l);
}

// 按当前语言返回文案：英语取 en，否则中文（中文是默认/回退）。
export const char* tr(const char* zh, const char* en) {
    return g_lang == cfg::Lang::En ? en : zh;
}

// 带占位符的格式化文案：tr() 选中模板后按运行时语言格式化（std::format 要求
// 编译期格式串，运行时模板只能用 std::vformat）。
export template <typename... Args>
std::string trf(const char* zh, const char* en, Args&&... args) {
    // make_format_args 只接受 lvalue（参数在 args 包里就是具名 lvalue，不能
    // forward 成右值——否则临时量/右值实参会实例化失败）。
    return std::vformat(tr(zh, en), std::make_format_args(args...));
}
