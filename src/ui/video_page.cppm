// ui/video_page.cppm — 视频解析页：粘贴视频网页链接 → yt-dlp 解析 → 选画质 →
// aria2 下载（DASH 自动 ffmpeg 合并）。首个支持站点 bilibili。
//
// 线程模型：解析走后台 detached 线程（resolveVideoUrl 同步阻塞，最长
// 60s），结果 经互斥量保护的 mailbox + requestUiUpdate() 回到 UI
// 线程消费；g_videoUrlText / g_videoInfo / g_videoError 等视图状态只允许 UI
// 线程读写（同 store.ui 纪律）。
module;

#include "eui_ui.h"

export module tinynext.ui.video_page;

import std;
import tinynext.config;
import tinynext.download_engine;
import tinynext.i18n; // tr / trf
import tinynext.ui.theme;
import tinynext.ui.utils; // 布局常量 + formatBytes/trimText（转发自 tinynext.utils）
import tinynext.ui.widgets;
import tinynext.ui.platform;    // getClipboardText
import tinynext.store.tasks;    // g_tasks.startVideoDownload
import tinynext.store.ui;       // showStatus / g_page_view / 筛选
import tinynext.video_resolver; // video::resolveVideoUrl / VideoInfo / VideoFormat

namespace {

// ---- 视图状态（仅 UI 线程读写）----
std::string g_videoUrlText;                  // 链接输入框
std::optional<video::VideoInfo> g_videoInfo; // 最近一次解析成功结果
std::string g_videoError;                    // 最近一次解析失败原因
int g_selectedFormat = 0;                    // 画质选择索引
bool g_qualityOpen = false;                  // 画质选择器弹层

// ---- 解析 mailbox（后台线程写 / UI 线程取，仅此一处跨线程）----
std::mutex g_parseMutex;
std::optional<video::ResolveResult> g_parseResult; // 待消费的解析结果
std::atomic<bool> g_parsing{false};
std::atomic<std::uint64_t> g_parseGen{0}; // 代数：丢弃过期的旧解析结果

// 配置默认画质 → 格式索引：label 包含配置串即命中，否则 0（最高画质，formats
// 已按 高度降序）。
int defaultFormatIndex(const video::VideoInfo &info) {
  const std::string want = cfg::videoConfig().defaultQuality;
  if (!want.empty()) {
    for (int i = 0; i < static_cast<int>(info.formats.size()); ++i) {
      if (info.formats[i].label.find(want) != std::string::npos)
        return i;
    }
  }
  return 0;
}

// 截断为单行可容纳的串。按字符实际宽度估算（CJK 全角 ≈ 1.0×字号，拉丁/数字 ≈
// 0.55×，即 eui 默认排版比例），累计超过可用宽就截断加省略号——保证不会换行压到
// 下面画质行，同时中文标题不无谓截短。用 eui
// 真实字体度量（ui.utils::ellipsizeText） 截断，与渲染一致、无估算偏差。
std::string fitSingleLine(const std::string &s, float widthPx, float fontSize) {
  return ellipsizeText(s, widthPx, fontSize);
}

// 启动一次解析（UI 线程调用；空串/非 http 直接提示，不启动线程）。
void startParse() {
  if (g_parsing.load())
    return;
  const std::string url = trimText(g_videoUrlText);
  if (url.empty()) {
    showStatus(tr("请输入视频页链接", "Please enter a video page URL"));
    return;
  }
  if (!url.starts_with("http://") && !url.starts_with("https://")) {
    showStatus(tr("视频解析仅支持 http(s) 链接",
                  "Video parsing only supports http(s) links"));
    return;
  }
  g_videoError.clear();
  g_parsing.store(true);
  const std::uint64_t gen = g_parseGen.fetch_add(1) + 1;
  const std::string cookie = cfg::videoConfig().bilibiliCookie;
  std::thread([url, cookie, gen] {
    video::ResolveResult r = video::resolveVideoUrl(url, cookie);
    // 只落地最新一代的结果：旧线程晚返回时直接丢弃（也不清 parsing 标志——
    // 那是新一代的事）。
    if (gen == g_parseGen.load()) {
      {
        std::lock_guard<std::mutex> lock(g_parseMutex);
        g_parseResult = std::move(r);
      }
      g_parsing.store(false);
    }
    core::platform::requestUiUpdate();
  }).detach();
}

} // namespace

// ===================== 视频页 =====================
// 布局：整页一张浮岛卡（无子侧边栏），从上到下：链接输入行 / 状态行 / 解析结果
// （标题 + 画质选择 + 下载按钮 + 提示）。
export void drawVideoPage(eui::Ui &ui, const eui::Screen &screen,
                          const AppTheme &theme) {
  // 消费解析结果（mailbox → UI 线程状态）。
  {
    std::optional<video::ResolveResult> pending;
    {
      std::lock_guard<std::mutex> lock(g_parseMutex);
      pending = std::move(g_parseResult);
      g_parseResult.reset();
    }
    if (pending.has_value()) {
      if (pending->ok && pending->info.has_value() &&
          !pending->info->formats.empty()) {
        g_videoInfo = std::move(*pending->info);
        g_videoError.clear();
        g_selectedFormat = defaultFormatIndex(*g_videoInfo);
        showStatus(trf("解析成功：{}", "Resolved: {}", g_videoInfo->title));
      } else {
        g_videoInfo.reset();
        g_videoError =
            pending->ok ? std::string(tr("未找到可下载的视频流",
                                         "No downloadable video streams found"))
                        : pending->error;
      }
    }
  }

  // 首次进入时剪贴板预填（对齐添加下载弹窗）：输入框为空且剪贴板是 http(s)
  // 链接。
  static bool clipChecked = false;
  if (!clipChecked) {
    clipChecked = true;
    if (g_videoUrlText.empty()) {
      const std::string clip = trimText(getClipboardText());
      if (clip.starts_with("http://") || clip.starts_with("https://")) {
        g_videoUrlText = clip;
      }
    }
  }

  const auto &tokens = theme.components;

  // ---- 布局尺寸（整页一张浮岛卡，同下载页风格）----
  const float islandTop = kIslandVInset;
  const float islandH = screen.height - 2.0f * kIslandVInset;
  const float islandX = kRailWidth;
  const float islandW = screen.width - islandX - kRightMargin;
  const float pad = kPanelPad;
  const float contentX = islandX + pad;
  const float contentW = islandW - 2.0f * pad;

  drawPanel(ui, "video.island", islandX, islandTop, islandW, islandH, theme);

  // 底部提示条：固定浮岛底缘，画质下拉在中间展开，不会再盖住提示文字。
  const float footerY = islandTop + islandH - pad - 16.0f;
  auto footerHint = [&](const std::string &text) {
    components::text(ui, "video.footer")
        .position(contentX, footerY)
        .size(contentW, 16.0f)
        .text(text)
        .fontSize(11.0f)
        .lineHeight(16.0f)
        .maxWidth(contentW)
        .color(theme.metaText)
        .build();
  };

  // ---- 标题 ----
  components::text(ui, "video.title")
      .position(contentX, islandTop + pad)
      .size(contentW, 18.0f)
      .text(tr("视频解析", "Video parser"))
      .fontSize(13.0f)
      .lineHeight(18.0f)
      .color(theme.titleText)
      .build();

  // ---- 链接输入行：URL 输入 + 解析按钮 ----
  const float urlY = islandTop + pad + 30.0f;
  const float parseBtnW = 76.0f;
  const bool parsing = g_parsing.load();
  components::input(ui, "video.url")
      .position(contentX, urlY)
      .size(contentW - parseBtnW - 10.0f, kInputHeight)
      .placeholder(tr("粘贴 YouTube / bilibili 等视频页链接…",
                      "Paste a YouTube / bilibili / other video page URL…"))
      .value(g_videoUrlText)
      .fontFamily("") // 用应用字体（Noto Sans SC）
      .theme(tokens)
      .onChange([](const std::string &v) { g_videoUrlText = v; })
      .onEnter([] { startParse(); })
      .build();
  components::button(ui, "video.parse")
      .position(contentX + contentW - parseBtnW, urlY)
      .size(parseBtnW, kInputHeight)
      .text(parsing ? tr("解析中…", "Parsing…") : tr("解析", "Parse"))
      .fontSize(12.0f)
      .theme(tokens, true)
      .textColor(onPrimaryColor(theme))
      .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
      .disabled(parsing)
      .onClick([] { startParse(); })
      .build();

  // ---- 状态 / 错误行 ----
  const float statusY = urlY + kInputHeight + 14.0f;
  if (parsing) {
    components::text(ui, "video.parsing")
        .position(contentX, statusY)
        .size(contentW, 16.0f)
        .text(tr("正在解析…（解析器首次启动较慢，最长约 60 秒）",
                 "Parsing… (the parser can take up to ~60s on first run)"))
        .fontSize(11.0f)
        .lineHeight(16.0f)
        .color(theme.metaText)
        .build();
    footerHint(tr("支持 YouTube / bilibili 等站点（yt-dlp 驱动）",
                  "Supports YouTube / bilibili and more (yt-dlp)"));
  } else if (!g_videoError.empty()) {
    components::text(ui, "video.error")
        .position(contentX, statusY)
        .size(contentW, 16.0f)
        .text(g_videoError)
        .fontSize(11.0f)
        .lineHeight(16.0f)
        .maxWidth(contentW)
        .color(theme.failed)
        .build();
  }

  // ---- 解析结果：标题 + 画质选择 + 下载按钮 ----
  if (g_videoInfo.has_value() && !parsing) {
    const video::VideoInfo &info = *g_videoInfo;
    const float titleY = statusY + 34.0f;
    components::text(ui, "video.name")
        .position(contentX, titleY)
        .size(contentW, 18.0f)
        .text(fitSingleLine(info.title, contentW, 12.0f))
        .fontSize(12.0f)
        .lineHeight(18.0f)
        .maxWidth(contentW) // 单行：即使截断估算偏差也不会换行叠到下面画质行
        .color(theme.nameText)
        .build();

    // 画质选择器：label + 后缀（容器 / 估算大小 / 是否需合并）。每个 label
    // 截断到 选择器单行能显示的长度——否则长文案（如 "2160p · webm · ~342.0 MB ·
    // 需合并"） 在字段里换行，第二行残影叠在字段底部。
    std::vector<std::string> labelStorage;
    std::vector<const char *> labels;
    labelStorage.reserve(info.formats.size());
    labels.reserve(info.formats.size());
    for (const auto &f : info.formats) {
      std::string label = f.label.empty()
                              ? (f.height > 0 ? std::to_string(f.height) + "P"
                                              : tr("未知画质", "Unknown"))
                              : f.label;
      label += " · " + (f.ext.empty() ? std::string("mp4") : f.ext);
      if (f.filesizeApprox > 0) {
        label += " · ~" + formatBytes(f.filesizeApprox);
      }
      if (!f.audioUrl.empty()) {
        label += tr(" · 需合并", " · merge");
      }
      labelStorage.push_back(fitSingleLine(std::move(label), 184.0f, 11.0f));
      labels.push_back(labelStorage.back().c_str());
    }
    g_selectedFormat = std::clamp(g_selectedFormat, 0,
                                  static_cast<int>(info.formats.size()) - 1);

    const float qualityY = titleY + 26.0f;
    // id 不能叫 "video.quality.label"：buildListPicker("video.quality") 内部字段文本
    // 也是 id+".label"，同名会被 eui 当同一图元（位置/对齐互相覆盖 → 选择文字残影）。
    components::text(ui, "video.quality.caption")
        .position(contentX, qualityY)
        .size(40.0f, kInputHeight)
        .text(tr("画质", "Quality"))
        .fontSize(12.0f)
        .lineHeight(kInputHeight)
        .color(theme.metaText)
        .build();
    ui.stack("video.quality.wrap")
        .position(contentX + 44.0f, qualityY)
        .size(220.0f, kInputHeight)
        .zIndex(30)
        .content([&] {
          buildListPicker(
              ui, "video.quality", 220.0f, kInputHeight, theme, g_qualityOpen,
              labels.data(), static_cast<int>(labels.size()), g_selectedFormat,
              false, PickerField::Text, [](int i) { g_selectedFormat = i; });
        })
        .build();

    // 下载按钮：选中的画质交给 TaskStore（合流单任务 / DASH 走 MergeTracker）。
    const float dlBtnW = 76.0f;
    components::button(ui, "video.download")
        .position(contentX + contentW - dlBtnW, qualityY)
        .size(dlBtnW, kInputHeight)
        .text(tr("下载", "Download"))
        .fontSize(12.0f)
        .theme(tokens, true)
        .textColor(onPrimaryColor(theme))
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([] {
          const auto &info = *g_videoInfo;
          dl::StartOptions opts; // 连接数 0 = 用配置默认；目录用全局下载目录
          const StartResult res = g_tasks.startVideoDownload(
              info, info.formats[g_selectedFormat], opts);
          showStatus(res.message);
          if (res.ok) {
            // 跳到下载列表看进度。
            g_page_view = Page::Downloads;
            g_filter = Filter::All;
          }
        })
        .build();

    // 底部提示：优先 DASH 合并说明；合流但少了 cookie 时提示 bilibili 登录
    // 能解锁高画质（仅 bilibili 链接显示）；否则显示通用站点支持。
    const video::VideoFormat &sel = info.formats[g_selectedFormat];
    const bool isBilibili =
        info.webpageUrl.find("bilibili.com") != std::string::npos;
    if (!sel.audioUrl.empty()) {
      footerHint(tr("该画质为音视频分离流，下载完成后自动合并为 mp4",
                    "This quality uses separate A/V streams; they are merged "
                    "into mp4 after download"));
    } else if (isBilibili && cfg::videoConfig().bilibiliCookie.empty()) {
      footerHint(
          tr("1080P+ / 会员画质需在「设置 → 视频」填写 bilibili SESSDATA",
             "1080p+ / member quality needs a bilibili SESSDATA in Settings → "
             "Video"));
    } else {
      footerHint(tr("支持 YouTube / bilibili 等站点（yt-dlp 驱动）",
                    "Supports YouTube / bilibili and more (yt-dlp)"));
    }
  } else if (!parsing && g_videoError.empty()) {
    // 空状态引导。
    components::text(ui, "video.guide")
        .position(contentX, statusY)
        .size(contentW, 16.0f)
        .text(tr("粘贴视频页链接后点击「解析」。支持 YouTube / bilibili "
                 "等（yt-dlp 驱动）",
                 "Paste a video page URL and hit Parse. Supports YouTube / "
                 "bilibili and more (yt-dlp)"))
        .fontSize(11.0f)
        .lineHeight(16.0f)
        .color(theme.metaText)
        .build();
    footerHint(
        tr("DASH 音视频分离自动合并为 mp4；高画质可能需要站点登录 cookie",
           "DASH A/V streams auto-merge to mp4; high quality may need site "
           "login cookies"));
  }
}
