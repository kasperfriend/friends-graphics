# Plan for "Inherit Global" Toggle Implementation

1. **`src/Environment/LocationTuning.h`**:
   - Add inheritance scope helper:
     ```cpp
     inline std::wstring InheritGlobalScope(unsigned level) {
         return Scope(level) + L".InheritGlobal";
     }
     ```
   - Section for INI persistence: `[<Scope>.InheritGlobal]` with keys `<section>.<key>=1` (lowercase keys).
   - Functions:
     - `bool HasInheritGlobal(LPCWSTR section, LPCWSTR key)`: checks if the active location scope (Area or Zone) has inherited global for this key.
     - `bool SetInheritGlobal(LPCWSTR section, LPCWSTR key, bool inherit)`: writes `1` or deletes the key in the current location scope (`Scope(editZone||!active.areaId?1:2)+L".InheritGlobal"`).
     - `bool HasInheritGlobalTab(int tabIndex)`: checks if all settings in the tab have `HasInheritGlobal == true`.
     - `bool SetInheritGlobalTab(int tabIndex, bool inherit)`: sets `SetInheritGlobal` for all settings in the tab.
   - Hierarchy resolution:
     - In `ReadString`:
       If `!IsGlobal(section) && path && baseFile == path && HasGlobalValue(section, key)`:
       Only override if NOT in location mode (`!editable || !active.valid`), OR if `HasInheritGlobal(section, key)` is true!
     - In `ReadEditorInt`:
       When in location scope (not `editGlobal`), if `HasInheritGlobal(section, key)` is true, return the inherited global value (if present, else fallback).
     - In `ScopedFingerprint`:
       Mix in `InheritGlobal` section values for each level so fingerprint caches correctly invalidate and detect changes.

2. **`src/Environment/OverlayLayout.h`**:
   - Add `Box InheritToggle(int index) const`: `{b.x + b.w - 260, b.y + 6, 52, 24}`.
   - Add `Box InheritGlobalTabButton() const`: `{ResetButton().x - 166, panel.y + 202, 158, 26}` (shares geometry with `ClearOverridesButton`).

3. **`TuningOverlay.h`**:
   - `CanInheritGlobalTab()` / `CanInheritGlobalRow()`: true when `editable && !renderer::locationtuning::editGlobal && !renderer::locationtuning::IsGlobal(...)`.
   - Update mouse click handling in `Update()`:
     - Clicking `InheritGlobalTabButton()` toggles inheritance for the tab.
     - Clicking `InheritToggle(index)` toggles inheritance for that row.
   - In `Draw()`:
     - Render `InheritGlobalTabButton` with label `INHERIT GLOBAL` (highlighted when all items in tab inherit).
     - In rows when `CanInheritGlobalRow()` is true: render `InheritToggle` box with label `INH` or `GLOBAL` in green (when ON) or muted (when OFF).

4. **Testing**:
   - Update `tools/OverlayLayoutRegression.cpp` to verify `InheritToggle` and `InheritGlobalTabButton`.
   - Update `tools/OverlayStateRegression.cpp` to test tab reset and scope behaviors.
   - Update `tools/LocationTuningCacheRegression.cpp` to test `InheritGlobal` INI persistence and precedence.
   - Verify `tools/LocationTuningCrossingRegression.cpp` performance budgets.
