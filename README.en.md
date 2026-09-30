# TinyNext Downloader

[简体中文](README.md) | **English**

TinyNext is a cross-platform desktop downloader written in C++23. It uses the
EUI-NEO GUI framework and runs **aria2-next** as its download engine. The
project builds with the `mcpp` package manager.

Supported input sources include HTTP(S), SFTP, ED2K file links, magnet links,
and local `.torrent` files. aria2-next also provides native HLS/DASH downloads,
multi-connection transfers, pause and resume, and persistent session recovery.

## Build and Run

```bash
mcpp build          # Build the application
mcpp run            # Launch the GUI
```

Downloads are saved to the system Downloads folder by default. Change the
destination in Settings.

## Command Line and Single Instance

- **Single instance:** TinyNext forwards links from later launches to the
  running instance instead of opening another window. On Windows, it also
  brings the existing window to the foreground.
- **Add downloads:** `tinynext <source>` opens the application if needed and
  adds the source. Multiple sources can be passed at once.
- Accepted sources are HTTP(S), SFTP, ED2K file links, `magnet:` links, and local
  `.torrent` paths. See [`docs/cli.md`](docs/cli.md) for full CLI usage.
- Run `tinynext agent` to print CLI instructions intended for AI assistants.
- The project guidelines for contributors and coding agents are in
  [`AGENTS.md`](AGENTS.md).

## Interface

TinyNext uses a monochrome visual style with mirrored light and dark themes.
The layout relies on neutral gray surfaces, thin outlines, and restrained
status colors. JetBrains Mono is used for numeric details such as speed,
progress, and version numbers.

- **Downloads:** Filter tasks by all, active, or completed; sort by date, state,
  name, size, or progress; and navigate pages from the bottom toolbar.
- **Task cards:** Show the file name, state, progress, size, speed, and task
  actions.
- **Add Download:** Enter one or more URLs, set a connection count, rename the
  output, choose a destination, or select a local torrent file. Multiple
  compatible URLs can be combined as mirror sources for one task. Choose
  automatic media detection or force HLS/DASH, select MP4/MKV output, and
  optionally pause magnet links after metadata so you can select torrent files.
- **Media tasks:** Show presentation-time progress for HLS/DASH downloads. Live
  recordings can be finalized from the task card.
- **Settings:** Configure the theme, download directory, connection behavior,
  network options, BitTorrent options, file handling, and integrity checks.
  The Engine tab shows aria2-next health and statistics and provides controls
  to check or restart the engine.

## UI Scaling

The application uses EUI-NEO's global `uiScale` setting. The `kUI` value in
`src/ui/utils.cppm` is the single scale control; component dimensions are
written in design pixels. The window also enforces a minimum size so the
responsive layout remains usable.

## Basic Usage

1. Click **＋** and enter an HTTP(S) or SFTP URL, an ED2K file link, a magnet link, or
   choose a local `.torrent` file.
2. Optionally set the connection count, output name, and destination directory.
   For torrent and magnet tasks, the content name comes from the torrent
   metadata.
3. Use the task card buttons to pause, resume, cancel, retry, open the
   downloaded file, or open its containing folder.

The toolbar can pause or resume all tasks. File name collisions follow the
configured engine policy.

On Linux, the system tray requires a desktop environment with a compatible
StatusNotifierItem (SNI) service.

## Pause, Resume, and Recovery

Task controls use aria2-next's JSON-RPC interface. TinyNext saves and restores
the engine session so unfinished tasks can be recovered after an application
or engine restart. TinyNext's session file and aria2 log are kept in the
per-user TinyNext configuration directory. aria2-next 2.8.3 stores HTTP,
BitTorrent, ED2K, and media recovery data in `aria2-state/` under that directory;
retries preserve the original task GID and output path.

## Settings

The Settings page groups options into tabs. Changes are staged until you save
them; daemon-level options take effect after the engine restarts.

- **General:** Theme, close-to-tray behavior, startup retry behavior, and the
  default download directory.
- **Downloads and network:** Split count, connections per server, minimum split
  size, speed limits, proxy, retry policy, User-Agent, Referer, custom headers,
  and cookie files.
- **Peer-to-peer:** BitTorrent seeding time and ratio, maximum peers, listening
  port, local peer discovery, extra trackers, and ED2K network settings.
- **File behavior:** Concurrent download count, file allocation, automatic
  renaming, overwrite policy, completion command, and disk cache.
- **Integrity:** Integrity checking and checksum configuration.
- **Engine:** aria2-next health, live statistics, engine restart, and logs.

Configuration and session files are stored in the per-user configuration
directory: `%APPDATA%\\TinyNext` on Windows,
`~/Library/Application Support/TinyNext` on macOS, or
`$XDG_CONFIG_HOME/tinynext` on Linux (falling back to `~/.config/tinynext`).

## Download Engine

The UI uses the `dl::DownloadEngine` interface in
[`src/download_engine.cppm`](src/download_engine.cppm). Its only implementation
is `dl::Aria2Engine` in `src/aria2_engine.cppm` and `src/aria2_engine.cpp`.
TinyNext launches the bundled `engines/aria2-next` executable and communicates
with it through a local JSON-RPC socket. The RPC listener is bound to localhost
and protected by a generated secret.

The engine release currently pinned by CI is aria2-next **2.8.3**. CI downloads
the platform binary and verifies it against `engines/checksums.sha256` before
packaging it with the application.

## Platforms and Engine Binaries

Release builds currently target Windows x64, Linux x86_64, and macOS Apple
Silicon. The `engines/` directory is ignored by Git; CI fetches the matching
aria2-next binary during release builds.

| Platform | aria2-next asset | Packaged as |
| --- | --- | --- |
| Windows x64 | `aria2-next-2.8.3-windows-x86_64.exe` | `engines/aria2-next.exe` |
| Linux x86_64 | `aria2-next-2.8.3-linux-x86_64` | `engines/aria2-next` |
| macOS Apple Silicon | `aria2-next-2.8.3-macos-arm64` | `engines/aria2-next` |

Download engine binaries from the [aria2-next releases page](https://github.com/AnInsomniacy/aria2-next/releases).

For local distribution builds, use `make-dist.ps1` on Windows or
`bash make-dist.sh <os> <arch>` on Linux and macOS. Linux launchers use the
system loader and Mesa; packaged builds require glibc 2.39 or newer.

## Releases

Pushing a `v*` tag triggers the GitHub Actions workflow in
`.github/workflows/release.yml`. It builds the supported platforms, packages
the application with aria2-next, and publishes a GitHub Release. A manual
`workflow_dispatch` run builds artifacts without publishing a release.

Release packages include installers or archives for the supported platforms.
See the [Releases page](https://github.com/FarnaHerry/tinynext/releases) for
downloads and release notes.

## Project Structure

TinyNext is organized into C++ modules:

| Module | File | Responsibility |
| --- | --- | --- |
| `tinynext.download_engine` | `src/download_engine.cppm` | Engine interface and task snapshots |
| `tinynext.aria2_engine` | `src/aria2_engine.cppm`, `src/aria2_engine.cpp` | aria2-next process and RPC integration |
| `tinynext.config` | `src/config.cppm` | Configuration, theme, and download paths |
| `tinynext.store.tasks` | `src/store/tasks.cppm` | Download task commands and domain state |
| `tinynext.store.ui` | `src/store/ui.cppm` | Page, filter, sort, and status state |
| `tinynext.store.dialogs` | `src/store/dialogs.cppm` | Dialog state and add/remove flows |
| `tinynext.ui.*` | `src/ui/*.cppm` | UI theme, widgets, pages, and platform helpers |
| Application entry | `src/app.cpp` | EUI app configuration and composition |

The EUI `app-main` package provides `main()`. Do not define another `main()` in
the application sources.

## Toolchain

| Component | Package | Version |
| --- | --- | --- |
| Toolchain | LLVM/Clang from `mcpp.toml` | 22.1.8 |
| UI framework | `compat:eui-neo` | See `mcpp.toml` |
| Download engine | aria2-next external process | 2.8.3 in the current CI workflow |
| JSON | `nlohmann:json` | 3.12.0 |

## License

TinyNext's source code is licensed under the MIT License; see [`LICENSE`](LICENSE).
The separately distributed aria2-next executable is licensed under GPL-2.0.
See [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) for third-party notices.
