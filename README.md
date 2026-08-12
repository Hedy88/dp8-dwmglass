# dp8-dwmglass

A work in progress open-source reimplementation of "Aero Glass for Windows 8.1"

## why?

The original is abandonware, hard to find and doesn't have much support for customization. Some implementations for things was reverse-engineered from using [Ghidra](https://github.com/nationalsecurityagency/ghidra) on the original [Aero 8](https://winmodpedia.miraheze.org/wiki/Aero_Glass_for_Win8%2B) binary.

I wanted something to use for a funny mod to Windows 8.1 that tries to bring the look of [Build 8102 (Developer Preview)](https://betawiki.net/wiki/Windows_8_build_8102.101) back

# status

### what currently works

- MinHook-based inline hooking of DWM render pass (CDrawingContext::DrawVisualTree)
- Symbol resolution via `symfetch_lib` (DbgHelp + MS symbol server, SHA-1-keyed
  local cache so hook install never needs the network after a pre-warm run)
- msstyles theme parsing via UxTheme (colors, glass params, frame images)
- Theme color extraction
- D3D11 glass renderer with behind-window capture
- Cached per-frame device resources (no per-frame allocation)
- Desktop Duplication API + GDI fallback
- 9-tap Gaussian blur shader
- Theme frame image compositing (aero.msstyles glass parts 11-14, tinted with the
  theme accent color) over the blurred glass quad
- Injector tool
- Symbol fetcher (`symfetch.exe`) - resolves DWM hook symbols into the
  `symfetch_lib` cache for offline hook installation

### todo

- Multi-pass Gaussian blur (vertical + horizontal)
- Present the composited glass to an actual output (DWM frame / swap chain)
- Glass safety zone implementation
- Caption text glow/shadow rendering
- Per-window glass enable/disable (needs window geometry without user32 on the
  DWM render thread)
- Debug logging / settings UI

# building

Builds on Windows (on another VM / machine is fine, tested on LTSC 2021). Requires:

- [Visual Studio 2019 or 2022 Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2019) with "Desktop development with C++"
- [premake5](https://premakedocs.github.io/download.html) - put `premake5.exe` on your `PATH` or in the repo root

> CI (`.github/workflows/build.yml`) builds on a `windows-2022` runner with `premake5 vs2022`.

## 1. get the code (including dependencies)

```cmd
git clone --recursive <repo-url> dp8-dwmglass
cd dp8-dwmglass
```

If you already cloned without `--recursive`, fetch the MinHook submodule with:

```cmd
git submodule update --init
```

## 2. generate the solution

```cmd
premake5 vs2019     :: for VS2019
premake5 vs2022     :: for VS2022
```

This creates `build\solutions\dp8-dwmglass.sln` (projects: `symfetch_lib` static lib, `dp8-dwmglass` DLL, `injector`, `symfetch`).

## 3. build

From an **x64 Native Tools Command Prompt** (VS 2019 or 2022, or after calling `vcvars64.bat`):

```cmd
msbuild build\solutions\dp8-dwmglass.sln /p:Configuration=Release /p:Platform=x64
```

or open `build\solutions\dp8-dwmglass.sln` in Visual Studio and build the Release/x64 configuration.

## 4. output

Binaries land in `build\bin\Release-windows-x86_64\`:

- `dp8-dwmglass.dll` - the injected DWM payload
- `injector.exe` - DLL injector
- `symfetch.exe` - DWM symbol downloader

Run the injector elevated from an **x64** console (must match `dwm.exe` bitness):

```cmd
injector.exe dp8-dwmglass.dll dwm.exe
```

## license

MIT License