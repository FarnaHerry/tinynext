---
name: tinynext-eui-ui
description: Build or modify TinyNext UI with EUI-NEO. Use for pages, dialogs, controls, responsive layout, and UI state or interaction bugs.
---

# TinyNext EUI UI Work

Use this skill for EUI-NEO changes in TinyNext. Read the repository `AGENTS.md` first; it defines the module layout, pinned EUI version, theme rules, state ownership, and build command. Check the current EUI documentation and the project wrappers in `src/ui/widgets.cppm` before relying on an API or composing a wrapped control.

## Element Identity

EUI uses resolved element IDs as runtime identity. IDs are not local to a `Stack`, `Column`, or `Row`; duplicate resolved IDs in the active tree can overwrite the element index and break drawing, hit testing, or retained state.

- Keep every simultaneously composed element ID unique, including IDs emitted inside `components::*` builders and helpers in `widgets.cppm`.
- Keep the same ID for the same logical element across composes. Avoid random IDs, timestamps, and array positions for data that can reorder or be filtered.
- Give collection rows a stable domain key. TinyNext examples: `task.id` for task cards, `TorrentFileView::index` for torrent files, and the opaque `MediaTrackView::id` for media tracks. Capture the key in callbacks and find the current item by key instead of capturing a vector offset.
- If a data item has no usable stable key, create a deterministic unique key before composing it. Do not emit selectable rows with duplicate IDs or missing backend identities.
- Namespace child IDs below the root (`root + ".part"`). Inspect the helper's generated suffixes before naming siblings: `buildListPicker` owns suffixes such as `.label`, `.field`, `.popup`, and `.dismiss`; `buildToggleSwitch` owns `.track` and `.thumb`.
- A numeric position is acceptable only when the data source defines it as the stable identity, as aria2's torrent file index does.
- In content callbacks that receive an `eui::Ui&`, build every child through that parameter. Measurement-based controls such as `scrollView` first invoke the callback with a temporary `Ui`, then compose it again into the live tree; using a captured outer `ui` duplicates elements and IDs.

## State And Callbacks

- Keep domain/task state in `store.tasks`; page and dialog state belongs in the corresponding view store or page module, following `AGENTS.md`.
- Use `ui.state<T>(id)` for EUI-owned component interaction state. Do not keep component state in a static map keyed by ID.
- Keep controlled values in their owning page/store and pass changes through the control callback. Do not expect a local variable recreated by `compose()` to persist.
- Capture stable values by value in element callbacks. If a callback needs EUI state, capture a pointer to an object owned by `ui.state`, and do not let it outlive that UI scope.
- Never add `.onFrame` to page composition. Use an event or owned worker and request a UI update when real state changes.

## TinyNext UI Conventions

- UI module files include the project's `eui_ui.h` shim; do not include `eui_neo.h` from a UI module.
- Reuse the wrappers in `src/ui/widgets.cppm` before adding a new control. Their internal suffixes are part of the ID namespace.
- Preserve the monochrome theme tokens, responsive layout rules, and dialog conventions in `AGENTS.md`.
- Build with `mcpp build`. For a UI task that asks for quick visual validation, inspect the actual window at the target size and correct visible clipping, overlap, or interaction issues before finishing.
