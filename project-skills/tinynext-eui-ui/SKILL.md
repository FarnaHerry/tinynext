---
name: tinynext-eui-ui
description: Build or modify TinyNext UI with EUI-NEO. Use for pages, dialogs, controls, responsive layout, and UI state or interaction bugs.
---

# TinyNext EUI UI Work

Use this skill for EUI-NEO changes in TinyNext. Project paths below are relative
to the repository root; reference links are relative to this skill.

## Read For The Task

Read the root [AGENTS.md](../../AGENTS.md) for global constraints and pinned versions.
Then load the references that apply to the change:

- For themes, pages, dialogs, task cards, fonts or scaling, read
  [design-system.md](references/design-system.md).
- For controls, IDs, state, callbacks, scroll containers, popups, inputs or
  animations, read [controls-runtime.md](references/controls-runtime.md).
- For module boundaries or state ownership, read
  [architecture.md](../../docs/architecture.md).
- For handles, processes or background workers, read
  [resource-lifecycle.md](../../docs/resource-lifecycle.md).

## Implementation

1. Inspect the relevant page, `src/ui/widgets.cppm`, and local documentation for
   the pinned EUI version before relying on an API or composing a wrapped control.
2. Reuse the project wrappers before adding a new control. UI modules include
   `eui_ui.h`; `src/app.cpp` uses the complete `eui_neo.h` for declarations.
3. Preserve the monochrome theme, vertical task progress, dedicated action row,
   outlined inputs and logical-pixel scaling defined by the design reference.
4. Keep domain state in `store.tasks`; put view and draft state in their owning
   view store or page. Use `ui.state<T>(id)` for EUI-owned interaction state.
5. Give every simultaneously composed element a unique, stable resolved ID.
   Inspect wrappers' generated suffixes and namespace children under their root.
6. Use stable domain keys for dynamic rows and callbacks. In measurement content
   callbacks, build through the supplied `eui::Ui&`, never a captured outer `ui`.
7. Keep overlays and dismiss layers outside clipping scroll containers; follow
   the control reference for picker placement and segmented alternatives.
8. Never add `.onFrame` to composition. Use events or owned workers and request
   a UI update only when real state changes; preserve the idle sleep baseline.

## Validation

- Build code changes with `mcpp build`; do not add a test executable with `main()`.
- For a UI task requesting quick visual validation, inspect the actual window at
  the target size and correct clipping, overlap and interaction issues.
- Match validation to the affected behavior: check stable IDs after reorder or
  filtering, popup dismissal and clipping, input focus and labels, or animation
  completion and return to idle as applicable.
