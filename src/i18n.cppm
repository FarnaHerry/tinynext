// i18n.cppm — UI 多语言（简中 / 繁中 / 英语）。
//
// 全量一等公民派：所有语言（含默认简中）都进表，调用点用短语义键 tr("key");
// 查当前语言列，查不到回退 zhCN，再查不到返回 key 本身。立即模式 UI 会重读 g_lang，
// 切换语言即刻生效（无需重启）。
//
// 无 eui 依赖：GUI / headless / store 消息都可 import 本模块。
export module tinynext.i18n;

import std;
import tinynext.config;
import tinynext.i18n_strings;

// 当前语言（模块级导出变量在 importers 间共享同一实体）。启动时读配置，运行时经
// setLanguage() 切换。
export cfg::Lang g_lang = cfg::lang();

// 切换语言：立即生效 + 持久化。
export void setLanguage(cfg::Lang l) {
    g_lang = l;
    cfg::setLang(l);
}

namespace {

const char* lookup(std::string_view key) {
    for (const auto& e : i18n::kEntries) {
        if (e.key == key) return e.zhCN.data();
    }
    return key.data();
}

const char* lookupZhTw(std::string_view key) {
    for (const auto& e : i18n::kEntries) {
        if (e.key == key) return e.zhTW.data();
    }
    return nullptr;
}

const char* lookupEn(std::string_view key) {
    for (const auto& e : i18n::kEntries) {
        if (e.key == key) return e.en.data();
    }
    return nullptr;
}

} // namespace

// 键查表：当前语言优先，缺失时回退简中，再缺失返回键本身。
export const char* tr(std::string_view key) {
    switch (g_lang) {
        case cfg::Lang::ZhTW: {
            if (const char* v = lookupZhTw(key); v) return v;
            [[fallthrough]];
        }
        case cfg::Lang::ZhCN:
            return lookup(key);
        case cfg::Lang::En: {
            if (const char* v = lookupEn(key); v) return v;
            return lookup(key);
        }
    }
    return key.data();
}

// 带占位符的格式化文案：tr() 选中模板后按运行时语言格式化（std::format 要求
// 编译期格式串，运行时模板只能用 std::vformat）。
export template <typename... Args>
std::string trf(std::string_view key, Args&&... args) {
    return std::vformat(tr(key), std::make_format_args(args...));
}
