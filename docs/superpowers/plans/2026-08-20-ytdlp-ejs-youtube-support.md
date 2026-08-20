# yt-dlp EJS YouTube Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update TinyNext's yt-dlp integration to use the 2026.08.19 official executable and reliably pass a system-installed Node runtime to YouTube extraction.

**Architecture:** `video_resolver.cppm` remains the sole yt-dlp command-construction boundary. It will normalize the configured or discovered Node executable into yt-dlp's `RUNTIME:PATH` syntax, keeping the actual runtime external to TinyNext. The release workflow downloads a version-pinned official yt-dlp executable, whose bundled EJS assets satisfy modern YouTube challenge solving without allowing remote components.

**Tech Stack:** C++23 modules, nlohmann::json, yt-dlp 2026.08.19 official executable, Node.js 24+, GitHub Actions, PowerShell, mcpp.

---

## File Map

- Modify: `src/video_resolver.cppm` — normalize configured/detected JavaScript runtime values and inject `--js-runtimes node:<path>`.
- Modify: `.github/workflows/release.yml` — pin yt-dlp asset fetches to the 2026.08.19 release.
- Modify: `THIRD-PARTY-NOTICES.md` — state the exact bundled yt-dlp version and EJS inclusion.
- Modify: `README.md` — document the system Node requirement for current YouTube parsing and configuration location.
- Replace locally, ignored: `engines/yt-dlp.exe` — official Windows 2026.08.19 release asset used for local verification.

## Constraints

- Do not bundle Node.js, Deno, Bun, or QuickJS.
- Do not add `--remote-components`; official yt-dlp executables bundle matching EJS assets.
- Do not change the aria2-next download architecture, DASH merge behavior, or bilibili cookie behavior.
- Do not add a `tests/` executable: `compat:eui-neo` with `app-main` creates a conflicting `main()` as documented in `CLAUDE.md`.
- Preserve the existing behavior of attempting non-YouTube resolution if no JS runtime is installed.

### Task 1: Normalize Node Runtime Selection

**Files:**
- Modify: `src/video_resolver.cppm:459-513`

- [ ] **Step 1: Record the existing resolver command shape as a manual failing reproduction**

Run from the repository root in PowerShell:

```powershell
$node = (Get-Command node.exe).Source
& .\engines\yt-dlp.exe --dump-single-json --no-warnings --no-check-certificates --no-update --no-playlist --js-runtimes "node:$node" "https://www.youtube.com/watch?v=BaW_jenozKc"
```

Expected before updating the engine: `engines/yt-dlp.exe` reports `2026.07.04`; the public sample may fail with `This video is unavailable`, proving only that the old executable cannot be used as the acceptance engine.

- [ ] **Step 2: Replace `detectJsRuntimeOnPath` with executable-path discovery**

In `src/video_resolver.cppm`, replace the current function that returns a runtime name with a helper that returns an explicit yt-dlp `RUNTIME:PATH` specification. It must search these pairs in order: `node/node`, `deno/deno`, `bun/bun`, `quickjs/qjs`, and `quickjs/quickjs`.

```cpp
std::string detectJsRuntimeSpecOnPath() {
#ifdef _WIN32
    const char* suffix = ".exe";
    const char sep = ';';
#else
    const char* suffix = "";
    const char sep = ':';
#endif
    const std::array<std::pair<const char*, const char*>, 5> candidates = {{
        {"node", "node"}, {"deno", "deno"}, {"bun", "bun"},
        {"quickjs", "qjs"}, {"quickjs", "quickjs"},
    }};
    // Split PATH, find the first existing executable, and return
    // runtime + ":" + utf8FromPath(executablePath).
}
```

Use `utf8FromPath()` for the executable path; Windows paths can contain non-ASCII characters.

- [ ] **Step 3: Add a dedicated configured-runtime normalization helper**

Add a `jsRuntimeSpec()` helper directly above `appendJsRuntimeArgs`. Its behavior must be exact:

```cpp
std::string jsRuntimeSpec() {
    const std::string configured = cfg::videoConfig().jsRuntime;
    if (configured.empty()) return detectJsRuntimeSpecOnPath();

    const std::filesystem::path candidate = pathFromUtf8(configured);
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
        return "node:" + utf8FromPath(candidate);
    }
    if (std::filesystem::is_directory(candidate, ec)) {
#ifdef _WIN32
        const std::filesystem::path node = candidate / "node.exe";
#else
        const std::filesystem::path node = candidate / "node";
#endif
        if (std::filesystem::is_regular_file(node, ec)) {
            return "node:" + utf8FromPath(node);
        }
    }
    // Preserve explicit yt-dlp specs such as "node:C:\\tools\\node.exe"
    // and supported symbolic names including "node".
    return configured;
}
```

This makes a full Node executable path usable from the Settings UI while retaining advanced yt-dlp forms such as `deno:C:\\path\\deno.exe`.

- [ ] **Step 4: Inject the normalized specification**

Replace `appendJsRuntimeArgs` with:

```cpp
void appendJsRuntimeArgs(std::vector<std::string>& args) {
    const std::string spec = jsRuntimeSpec();
    if (spec.empty()) return;
    args.push_back("--js-runtimes");
    args.push_back(spec);
}
```

Keep its call in `resolveVideoUrl` after the base yt-dlp options and before proxy/cookie/input URL arguments.

- [ ] **Step 5: Build TinyNext**

Run:

```powershell
mcpp build
```

Expected: successful build with no module or UTF-8/path conversion errors.

- [ ] **Step 6: Commit runtime normalization**

```bash
git add src/video_resolver.cppm
git commit -m "fix: pass explicit JS runtime to yt-dlp" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Pin the Official yt-dlp EJS-Capable Engine

**Files:**
- Modify: `.github/workflows/release.yml:276-283`
- Modify: `THIRD-PARTY-NOTICES.md:20-28`

- [ ] **Step 1: Update the release workflow URL**

Declare a shell-local release tag in the `Fetch yt-dlp engine` workflow step and replace `latest/download` with the exact 2026.08.19 asset URL:

```bash
set -euo pipefail
ytdlp_version="2026.08.19"
curl -fL --retry 3 -o "${{ matrix.ytdlp_asset }}" \
  "https://github.com/yt-dlp/yt-dlp/releases/download/${ytdlp_version}/${{ matrix.ytdlp_asset }}"
chmod +x "${{ matrix.ytdlp_asset }}"
mv "${{ matrix.ytdlp_asset }}" "${{ matrix.ytdlp_file }}"
```

Keep the platform-specific asset mapping unchanged: `yt-dlp.exe`, `yt-dlp_linux`, and `yt-dlp_macos`.

- [ ] **Step 2: Update third-party notices**

Replace the yt-dlp version line with:

```markdown
- **版本**：2026.08.19（官方 standalone executable，内含匹配的 yt-dlp-ejs 组件）
```

Replace the purpose line with:

```markdown
- **用途**：视频网页解析（出直链 / 请求头）；内置 yt-dlp-ejs 用于 YouTube JavaScript challenge 解析，仍需系统提供受支持的 JavaScript runtime。
```

Do not claim that TinyNext bundles Node.js.

- [ ] **Step 3: Validate the version-pinned URL without modifying the repository engine**

Run:

```powershell
$asset = Join-Path $env:TEMP "yt-dlp-2026.08.19.exe"
curl.exe -fL --retry 3 -o $asset "https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe"
& $asset --version
```

Expected output: `2026.08.19`.

- [ ] **Step 4: Commit packaging metadata**

```bash
git add .github/workflows/release.yml THIRD-PARTY-NOTICES.md
git commit -m "build: pin yt-dlp 2026.08.19" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 3: Verify Upstream EJS + Node Parsing and Local Engine Replacement

**Files:**
- Replace locally, ignored: `engines/yt-dlp.exe`
- Modify: `README.md:69-93`

- [ ] **Step 1: Replace the ignored local Windows engine with the validated official asset**

Run:

```powershell
Copy-Item "$env:TEMP\yt-dlp-2026.08.19.exe" ".\engines\yt-dlp.exe" -Force
& .\engines\yt-dlp.exe --version
```

Expected output: `2026.08.19`.

- [ ] **Step 2: Directly probe current yt-dlp using TinyNext's exact runtime syntax**

Run a public YouTube URL supplied by the developer, avoiding samples that YouTube has removed:

```powershell
$node = (Get-Command node.exe).Source
& .\engines\yt-dlp.exe --dump-single-json --no-warnings --no-check-certificates --no-update --no-playlist --js-runtimes "node:$node" "<public-youtube-url>" 2> "$env:TEMP\tinynext-ytdlp-ejs.stderr" | Set-Content "$env:TEMP\tinynext-ytdlp-ejs.json" -Encoding utf8
Get-Content "$env:TEMP\tinynext-ytdlp-ejs.stderr" -Tail 80
(Get-Content "$env:TEMP\tinynext-ytdlp-ejs.json" -Raw | ConvertFrom-Json).formats.Count
```

Expected: exit code `0`, a JSON object, and a positive `formats.Count`. The stderr output must not say that `yt-dlp-ejs` or a JavaScript runtime is unavailable.

- [ ] **Step 3: Update README video prerequisites**

After the paragraph that identifies yt-dlp as the video parser, add this concise paragraph:

```markdown
- **YouTube 解析依赖**：发行包附带的 yt-dlp 包含 EJS challenge solver；系统还需安装受支持的 JavaScript runtime。TinyNext 会优先从 PATH 自动找到 Node.js，也可在「设置 → 视频 → JavaScript runtime」填 `node`、`node:C:\\完整\\node.exe` 或 runtime 可执行文件的完整路径。推荐 Node.js 22+；未安装 runtime 时，bilibili 等不需要 JS challenge 的站点仍可尝试解析。
```

- [ ] **Step 4: Rebuild and run TinyNext headless resolver smoke test**

Run:

```powershell
mcpp build
$binDir = (Get-Content .\target\.build_cache)[1].Trim()
& (Join-Path $binDir "bin\tinynext.exe") --resolve "<public-youtube-url>"
```

Expected: process exit code `0`, printed title, and `qualities count` greater than zero. On a GUI-subsystem Windows build where console inheritance is unavailable, use the application Video page and verify it shows a non-empty quality picker instead.

- [ ] **Step 5: Confirm non-YouTube behavior remains available without an explicit runtime setting**

Clear `video.js_runtime` from `%APPDATA%\TinyNext\tinynext.conf` only in a disposable/local test configuration, then resolve a known public bilibili URL through the Video page. Restore the original configuration afterward. Expected: the resolver attempts extraction and returns its normal stream list or source-specific error; it must not reject the request merely because JavaScript runtime discovery is absent.

- [ ] **Step 6: Commit README documentation**

```bash
git add README.md
git commit -m "docs: document YouTube JS runtime requirement" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 4: Final Repository Verification

**Files:**
- Verify only

- [ ] **Step 1: Inspect the final diff and commit history**

Run:

```bash
git status --short
git log --oneline -4
git diff HEAD~3..HEAD -- src/video_resolver.cppm .github/workflows/release.yml THIRD-PARTY-NOTICES.md README.md
```

Expected: only the intended resolver, release pin, notice, and documentation changes are present. The ignored `engines/yt-dlp.exe` replacement must not appear in Git status.

- [ ] **Step 2: Run the final compilation verification**

Run:

```powershell
mcpp build
```

Expected: successful development build.

- [ ] **Step 3: Report exact evidence and residual external risk**

Record all of the following in the completion report:

```text
- Bundled local yt-dlp version: 2026.08.19
- Node executable and version used for the probe
- Direct yt-dlp probe exit code and format count
- TinyNext --resolve outcome
- mcpp build outcome
```

Also state that YouTube availability and challenge format are external and can change independently of the shipped app; a future yt-dlp update may be required.
