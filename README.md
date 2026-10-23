# Snip

A tiny, instant screenshot tool for Windows (C++ / Win32 / Direct2D, a single ~240 KB exe with no runtime).
It stays resident in the tray and replaces **Win+Shift+S** and **PrtSc**.

## Install

```
install.bat            # builds, copies to %LOCALAPPDATA%\Programs\Snip, registers the elevated logon task, starts it
build.bat [arm64|x64]  # build only -> build\<arch>\snip.exe
```

Requires Visual Studio 2022 with the C++ workload and a Windows SDK.

## Usage

Press **Win+Shift+S** or **PrtSc**. The screen (all monitors) freezes.

| Step | Action |
|---|---|
| Select | drag a region · click = window under cursor · `Space` = window/monitor under cursor · `F` = whole monitor · `Ctrl+A` = all monitors |
| Adjust | drag the handles or edges · `Move` tool (`V`) drags the selection · arrows nudge it (`Shift` ×10) · `Ctrl+arrows` resize |
| Draw | `P` pen · `L` line · `A` arrow · `R` rectangle · `E` ellipse · `M` marker · `T` text · `B` pixelate · `N` numbered badges |
| Mirror | `H` flips the selection left-right, `Shift+V` (or `Shift+H`) top-bottom; before selecting, it flips the highlighted window. Annotations follow, text stays readable, `Ctrl+Z` undoes |
| Style | `1`–`8` color · `[` `]` or `Ctrl+wheel` size · `Shift` snaps lines to 45° and makes squares/circles |
| Zoom | mouse wheel (up to 3200%) · middle-drag pans · `+`/`-` · `0` resets. The loupe shows pixels, coordinates and color |
| Finish | `Enter` / `Ctrl+C` / double-click = copy · `Ctrl+S` = save to `Pictures\Screenshots` (and copy) · `Ctrl+Shift+S` = save as · `Esc` = cancel |
| Other | `Ctrl+Z` / `Ctrl+Y` undo/redo · right-click resets the selection · `C` (before selecting) copies the pixel color as `#RRGGBB` |

While typing text: `Enter` commits, `Shift+Enter` adds a new line, `Ctrl+V` pastes.

The tray icon menu toggles the Win+Shift+S / PrtSc interception and autostart. Running `snip.exe` again triggers a capture
in the running instance; `snip.exe --exit` stops it.

## Notes

- Hotkeys use a low-level keyboard hook, so the Windows Snipping Tool never sees Win+Shift+S. Other Win+Shift
  shortcuts (e.g. PowerToys Text Extractor, Color Picker) are untouched.
- Autostart is a Task Scheduler logon task `Snip` with "run with highest privileges" (normal priority, no battery or
  time limits), so the hook also works while an admin window has focus. `install.bat` asks for UAC once; if declined it
  falls back to a non-elevated HKCU `Run` entry. The tray menu toggles it (asks for UAC when needed).
- Copy puts both `PNG` and `CF_DIB` on the clipboard.
- Debugging: `snip.exe --selftest <dir>` renders the overlay offscreen to PNGs with timings; `SNIP_TRACE=1` logs input
  to `%TEMP%\snip_trace.log`.
