# yt-dlp EJS YouTube Support Design

## Goal

Make TinyNext's YouTube video parsing work with yt-dlp 2026.08.19 and a system-installed Node.js runtime. The scope is resolver support only: TinyNext will continue to use yt-dlp to resolve streams and aria2-next to download them.

## Root Cause

TinyNext currently packages yt-dlp 2026.07.04. Although the resolver already appends `--js-runtimes node` when Node is discoverable, full current YouTube support requires the newer yt-dlp EJS challenge-solver component as well as a supported JavaScript runtime.

The 2026.08.19 official yt-dlp executable includes the required EJS component. The local Node 24.18.0 installation meets yt-dlp's Node 22+ requirement. Therefore updating the packaged yt-dlp binary is necessary; adding another TinyNext-side JavaScript command flag alone cannot repair the older engine.

## Design

### Runtime Selection

Keep the existing `cfg::VideoConfig::jsRuntime` setting and automatic PATH detection. The resolver will pass yt-dlp an explicit runtime specification:

- User setting accepts either a runtime name or an executable path.
- A configured executable path is encoded as `node:<absolute-path>` rather than passed as a bare runtime name.
- An unconfigured runtime uses the detected `node` executable path, not only the symbolic `node` name.
- If no compatible runtime is found, the resolver still invokes yt-dlp. Non-YouTube sites remain unaffected, while yt-dlp's stderr provides its own actionable failure detail.

This keeps the app dependent on a system Node installation, as selected by the user, while avoiding failures caused by a GUI process inheriting a different PATH from an interactive shell.

### yt-dlp Engine Update

The release workflow will continue downloading the official upstream executable, which includes yt-dlp-ejs. It will pin the download URL to the 2026.08.19 release instead of using the moving `latest` endpoint, making releases reproducible. Third-party notices will identify the exact bundled yt-dlp version.

For local development, the checked-in ignored `engines/yt-dlp.exe` is replaced manually with the official 2026.08.19 Windows executable. No Python source distribution or Python runtime is added to TinyNext.

### Resolver Invocation

`resolveVideoUrl` remains the single command-construction site. It continues to request one JSON object with no playlist and write stderr to the existing resolver log. It will supply the selected JavaScript runtime through `--js-runtimes` before the input URL.

No remote EJS component is enabled: the official executable contains its matching EJS files. This avoids adding a runtime network dependency and avoids executing an arbitrary remote component.

## Error Handling

- Missing Node does not prevent bilibili and other non-JS-challenge extraction from being attempted.
- A failed YouTube extraction returns the existing stderr-tail error through the UI.
- The existing 60-second cancellation-aware timeout remains unchanged.
- The resolver does not treat a runtime executable path as a runtime name; invalid paths result in yt-dlp's normal diagnostic rather than an invalid command shape.

## Verification

1. Validate the official 2026.08.19 executable reports its expected version.
2. Run yt-dlp directly against a public YouTube URL with the exact runtime argument TinyNext will generate; inspect stderr and JSON output.
3. Build TinyNext with `mcpp build`.
4. Run `tinynext --resolve <public-youtube-url>` and confirm a non-empty quality list is printed.
5. Confirm the release workflow now fetches the pinned engine version and third-party notices match it.

## Non-Goals

- Bundling Node.js, Deno, Bun, or QuickJS.
- Enabling yt-dlp remote EJS components.
- Changing TinyNext's aria2-next download architecture.
- Changing bilibili cookie or DASH merge behavior.
