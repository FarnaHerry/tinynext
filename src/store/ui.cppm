// store/ui.cppm — 视图 store：状态消息条 / 当前页面 / 列表筛选·排序·分页。
// 是「这个 UI 实现」的视图状态（换 UI 框架时随视图层一起重写），但本身不
// import eui——纯数据 + 纯函数，立即模式 UI 每帧读它重绘。
//
// 线程纪律：本模块全局只被 UI 线程读写；后台线程（housekeep 等）经原子标志
// / pending 队列中转（g_statusExpiry 是唯一例外：housekeep 只读，UI 线程写）。
export module tinynext.store.ui;

import std;
import tinynext.download_engine;  // dl::State（stateMatches 的任务状态入参）

// ---- 页面 ----

export enum class Page { Downloads, Settings };
export Page g_page_view = Page::Downloads;  // 默认打开下载列表

// ---- 状态消息条（4s 自动消失）----

export std::string g_statusMessage;
export float g_statusTimer = 0.0f;

// 用墙钟过期时间（原子）而不是靠每帧递减 g_statusTimer——后台 housekeep 线程
// 读它判断过期，UI 线程写。g_statusMessage/g_statusTimer 仍只由 UI 线程读写。
std::atomic<double> g_statusExpiry{0.0};
double steadyNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

export void showStatus(std::string message) {
    g_statusMessage = std::move(message);
    g_statusTimer = 4.0f;
    g_statusExpiry.store(steadyNow() + 4.0);
}

// housekeep 后台线程调用：状态消息是否已到 4s 过期时间。
export bool statusExpired() {
    const double expiry = g_statusExpiry.load();
    return expiry > 0.0 && steadyNow() >= expiry;
}

// ---- 列表筛选 / 排序 / 分页 ----

export enum class Filter { All, Active, Done };

// 下载列表：状态筛选 + 分页。snapshot() 最新在前，先按筛选收窄，再按
// 当前页切片。切换筛选或分页大小时回到第 1 页。
export Filter g_filter = Filter::All;
export int g_page = 1;
export int g_pageSize = 5;
export bool g_pageSizeOpen = false;  // 分页大小下拉是否展开
export constexpr int kPageSizes[] = {5, 10, 20, 50, 100};

// 下载列表排序。切换排序回到第 1 页。
export enum class SortMode { Newest, State, Name, Size, Progress };
export SortMode g_sort = SortMode::Newest;
export bool g_sortOpen = false;  // 排序下拉是否展开

// "下载中" = 排队/进行/暂停；"已完成" = 完成/失败/已取消。
export bool stateMatches(Filter filter, dl::State state) {
    switch (filter) {
        case Filter::All: return true;
        case Filter::Active:
            return state == dl::State::Queued || state == dl::State::Downloading ||
                   state == dl::State::Paused;
        case Filter::Done:
            return state == dl::State::Done || state == dl::State::Failed ||
                   state == dl::State::Cancelled;
    }
    return true;
}

export int pageSizeIndex() {
    for (int i = 0; i < 5; ++i) {
        if (kPageSizes[i] == g_pageSize) return i;
    }
    return 0;  // 5
}
