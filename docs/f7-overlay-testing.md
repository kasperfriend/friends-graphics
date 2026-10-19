# F7 overlay verification

The overlay has ten left-side category tabs: Atmosphere & God Rays, Local
Volumetric Fog, Weather Visuals, Wind, Volumetric Clouds, Water & Reflections,
Local Lighting, Dynamic Shadows, Ambient Occlusion, and Image & Color. Each
`KIND_HEADER` in `OverlayItems.h` makes one, so a new group of controls joins an
existing tab unless it is really a new subject: the seven underwater controls
are in Water & Reflections, because the medium they author is the same water
that tab already authors.
`OverlayItems.h` holds every control with its key, default, range and save
semantics - the original 69 plus the wind, cloud-resolution, water,
ambient-occlusion and underwater controls added since, less the eleven
vegetation-sway rows, the eleven global-illumination rows and the four unfinished
AI Materials rows that were removed with those features. The count is not written
down here because it is pinned where it can be checked:
`tools/OverlayLayoutRegression.cpp` walks the table and reports the number of
settings it reached (114 as of the AI Materials removal), so a row that cannot be
reached - a duplicate key, a control past the last header - fails the portable
test rather than this paragraph.
`OverlayLayout.h` is platform-independent and supplies the rectangles used by
both Direct3D drawing and Win32 hit testing, including the scrollbar and resize
handle. Window geometry and tab scroll positions last for
the client session; they are not written to the graphics preset.

## Automated regressions

Portable geometry, from the repository root:

```sh
mkdir -p build
g++ -std=c++20 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -g tools/OverlayLayoutRegression.cpp \
  -o build/overlay-test
./build/overlay-test
```

Runs in the Build workflow under **Portable regressions**, alongside the other
fast CPU tests.

The real overlay is also rendered on a system D3D9 device (Windows) by
`tools/Test-Overlay.cmd` (`tools/OverlayRegression.cpp`), next to
`tools/Test-OverlayState.cmd`. It covers the same tab/scroll hit-test sweep at
1080p, 1440p and 2160p on a real device, the panel fitting the backbuffer,
scope-button reachability, render-state restoration, font atlas creation and
device-reset recovery, and writes `build/overlay-preview/*.ppm` frames for
visual inspection. It exits 77 when the runner has no D3D9 HAL, which
`Test-Overlay.cmd` reports as a skip. Both device tests run in the Build
workflow's Windows job; the portable test is what guards the geometry on Linux
runners and forks.

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

1. Open F7 in windowed and fullscreen modes. Visit every tab and scroll
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
6. Check all three scopes: zone, subarea, and Global. An unavailable subarea
   cannot be selected; Global can always be selected. Opening Global must not
   create overrides. Edit one Global setting and confirm it wins over both
   location scopes, survives map changes and F12, and leaves other keys alone.
   In Global, CLEAR OVERRIDES restores saved location values for only the
   selected category; RESET DEFAULTS instead persists explicit global defaults.
   Image & color remains global-only through GraphicsEffects.ini. Reload with F12
   and confirm saved values persist. Confirm save failures are still reported.
7. Close with X and F7, including F7 during a drag, then reopen. Size and position
   should remain; no drag should resume. Opening while already holding the
   mouse button must not activate a control.
8. Alt+Tab during each kind of drag; release while unfocused, then return. Also
   change resolution/DPI and trigger a device reset. No gesture should remain
   stuck, the panel must remain on screen, and text should be recreated.
9. Close F7 and confirm normal game mouse input returns. If any runtime check
   fails, capture `MenuDiagnostics.log`, resolution/DPI and a screenshot.

The ranges, defaults and file targets of every control were also compared against
the code that reads them; that review, including the items still open, is in
[F7 menu audit](f7-menu-audit.md).
