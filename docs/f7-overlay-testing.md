# F7 overlay verification

The overlay now has nine left-side category tabs. `OverlayItems.h` holds the
original 69 controls with unchanged keys, defaults, ranges and save semantics.
`OverlayLayout.h` is platform-independent and supplies the rectangles used by
both Direct3D drawing and Win32 hit testing, including the scrollbar and resize
handle. Window geometry and tab scroll positions last for the client session;
they are not written to the graphics preset.

## Automated regression

From the repository root:

```sh
mkdir -p build
g++ -std=c++20 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -g tools/OverlayLayoutRegression.cpp \
  -o build/overlay-test
./build/overlay-test
```

Also runs in the Build workflow as **F7 tabs, dragging and resizing**.

Coverage:

- Every original control is reachable through its tab, including the last cloud
  control at minimum window size. Headers cannot be edited as settings.
- Row/control hit tests and exclusive right/bottom boundaries agree.
- Title dragging clamps to screen bounds without changing window size.
- Corner resizing maintains its top-left anchor, minimum dimensions and screen
  bounds; it does not overlap settings or scroll buttons.
- Cancellation stops a drag. Resolution/DPI changes cancel window gestures,
  clamp the panel and keep all tabs reachable.
- Per-tab scroll positions, scroll limits, thumb dragging, and removal of stale
  scroll offsets when growing the window.
- Slider range, step quantization, monotonicity and exact endpoints.
- 640x480 through 3840x2160 at 100%, 125%, 150% and 200% DPI; a further downsize
  to 320x240 stresses geometry containment. Client/backbuffer coordinate scaling
  is also checked.

The overlay header was additionally compiled as a 32-bit Windows object using
Zig 0.14.1's Clang/MinGW headers. This checks the Win32/D3D integration's C++
compilation, not its runtime behavior. A whole-renderer build with that compiler
is blocked by existing MSVC-specific wide-string file-stream constructors in
`NativeShadowDiagnostics.h` and `ModernWoWRenderer.cpp`; use the normal MSBuild
workflow for the full DLL.

## In-game acceptance checklist (requires Windows and WoW)

These runtime checks cannot be completed in the Linux development sandbox:

1. Open F7 in windowed and fullscreen modes. Visit all nine tabs and scroll
   Clouds to its last setting. Confirm labels, values and hints are legible and
   no hidden row receives clicks.
2. Drag the title in all directions, including beyond the client edges. Release
   outside the panel: movement must stop, and no camera rotation should occur.
3. Drag the bottom-right grip smaller/larger and against screen edges. The
   top-left anchor should remain fixed; the minimum size must keep tabs,
   controls, scope selectors and close button accessible.
4. Scroll with the wheel, thumb, track and arrow buttons. Switching tabs should
   preserve their individual scroll positions. Enlarging the window should
   clamp any scroll position which is no longer needed.
5. Click a slider label: its value must not jump. Drag the slider through both
   endpoints and outside its track. Toggles change once per press. Moving,
   resizing and scrolling must not change or save any setting.
6. Check both verified zone/subarea scopes and global fallback. An unavailable
   subarea cannot be selected. Image & color remains global. Reload with F12
   and confirm saved values persist. Confirm save failures are still reported.
7. Close with X and F7, including F7 during a drag, then reopen. Size and position
   should remain; no drag should resume. Opening while already holding the
   mouse button must not activate a control.
8. Alt+Tab during each kind of drag; release while unfocused, then return. Also
   change resolution/DPI and trigger a device reset. No gesture should remain
   stuck, the panel must remain on screen, and text should be recreated.
9. Close F7 and confirm normal game mouse input returns. If any runtime check
   fails, capture `MenuDiagnostics.log`, resolution/DPI and a screenshot.
