# Waifuland

> **Disclaimer:** This repository is "vibe coded". Please use with caution.

A Linux desktop Live2D model viewer that renders Live2D characters as transparent Wayland overlay windows using the `wlr-layer-shell` protocol.

Live2D models float on your desktop with click-through transparency — only the model itself receives input. Built with the Live2D Cubism SDK for Native, OpenGL (EGL), and native Wayland (no X11, no GLFW windowing).

<!-- Screenshot or GIF placeholder: place a demo image/gif here -->
<!-- ![Waifuland Demo](docs/demo.gif) -->


https://github.com/user-attachments/assets/95dfdedc-4957-4bb7-b79f-f3addd97a6db



## Table of Contents

- [Features](#features)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Usage](#usage)
- [Model Setup](#model-setup)
- [Architecture](#architecture)
- [Built With](#built-with)
- [License](#license)

## Features

- **Wayland-native overlay** — renders as a layer-shell surface, always on top of your desktop
- **Click-through transparency** — only the Live2D model area receives pointer events; everything else passes through
- **Multi-compositor support** — works on any Wayland compositor supporting `wlr-layer-shell` (Hyprland, Sway, river, etc.)
- **Multi-output support** — switch between monitors (Hyprland)
- **Interactive** — responds to mouse drag, tap, and scroll input
- **Motion & expression** — supports idle animations, lip-sync, eye-blink, physics, and expressions
- **Configurable model directory** — load models from any path via CLI flag or XDG config

## Prerequisites

| Dependency | Notes |
|---|---|
| **Linux with Wayland compositor** | Must support `wlr-layer-shell-v1` (Hyprland, Sway, river, etc.) |
| **Live2D Cubism SDK for Native** | Download from [live2d.com/sdk](https://www.live2d.com/en/sdk/about/) (proprietary, not bundled) |
| **C++ compiler** | GCC or Clang with C++14 support |
| **CMake** | >= 3.16 |
| **pkg-config** | For finding Wayland/EGL libraries |
| **Wayland development libraries** | `wayland-client`, `wayland-egl`, `wayland-cursor` |
| **EGL & OpenGL** | `libegl-dev`, `libgl-dev` or equivalent |
| **wayland-scanner** | Usually part of `wayland-protocols` or `wayland` dev packages |
| **curl, unzip** | For downloading third-party dependencies (GLEW) |

### Installing system dependencies

**Arch Linux:**
```bash
sudo pacman -S --needed base-devel cmake pkgconf wayland wayland-protocols libglvnd egl-wayland curl unzip
```

**Ubuntu / Debian:**
```bash
sudo apt install build-essential cmake pkg-config libwayland-dev wayland-protocols libegl-dev libgl-dev curl unzip
```

**Fedora:**
```bash
sudo dnf install gcc-c++ cmake pkgconf-pkg-config wayland-devel wayland-protocols-devel mesa-libEGL-devel mesa-libGL-devel curl unzip
```

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/shinkuan/waifuland.git
cd waifuland
```

### 2. Download the Live2D Cubism SDK

1. Go to [https://www.live2d.com/en/sdk/about/](https://www.live2d.com/en/sdk/about/)
2. Download **Cubism SDK for Native**
3. Extract it into the project root directory:
   ```
   waifuland/
   ├── CubismSdkForNative-5-r.5/   <-- extracted SDK here
   ├── CMakeLists.txt
   ├── src/
   └── ...
   ```
4. If the folder name differs from `CubismSdkForNative-5-r.5`, update `SDK_ROOT_PATH` in `CMakeLists.txt`.

### 3. Build

Use the install script (recommended):

```bash
chmod +x install.sh
./install.sh
```

Or build manually:

```bash
cd thirdParty && bash scripts/setup_glew && cd ..
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

The binary will be at `build/bin/waifuland`.

## Usage

```bash
# Run with default model directory (~/.config/waifuland/models/)
./build/bin/waifuland

# Run with a custom model directory
./build/bin/waifuland --models_dir /path/to/your/models
```

### Controls

| Input | Action |
|---|---|
| **Left-click Upper Body** | Trigger expression change (if any) |
| **Left-click Lower Body** | Trigger motion (if any) | 
| **Left-click drag** | Drag the model / trigger hit areas |
| **Right-click** | Switch to next model |
| **Middle-click** | Switch skin (if any) |
| **Scroll wheel** | Zoom in/out |

### Compositor Compatibility

Waifuland runs on any Wayland compositor that supports the `wlr-layer-shell` protocol. Some features require compositor-specific IPC and are only available on Hyprland:

| Feature | Hyprland | Sway | Other wlroots |
|---|---|---|---|
| Overlay rendering | Yes | Yes | Yes |
| Click-through transparency | Yes | Yes | Yes |
| Mouse drag / tap / scroll | Yes | Yes | Yes |
| Global cursor tracking (eyes follow cursor anywhere) | Yes | No | No |
| Cross-monitor drag | Yes | No | No |
| Move to focused monitor | Yes | Yes | No |

The compositor is auto-detected at startup. Feature availability is logged to the console.

### Toggling Visibility

You can hide and show your Live2D model (along with its click-through input region) by sending a `SIGUSR1` signal to the background process. When hidden, it uses virtually no resources.

```bash
kill -SIGUSR1 $(pgrep -x waifuland)
# Or
killall -s SIGUSR1 waifuland
# Or using the included toggle command:
./build/bin/waifuland toggle
```

### Move to Focused Monitor

Move the model to whichever monitor currently has focus by sending a `SIGUSR2` signal. This works on Hyprland and Sway.

```bash
kill -SIGUSR2 $(pgrep -x waifuland)
# Or
killall -s SIGUSR2 waifuland
# Or using the included focus command:
./build/bin/waifuland focus
```

**Hyprland keybind examples**  
Add these to your `~/.config/hypr/hyprland.conf`:
```ini
# Toggle waifuland visibility with Super + W
bind = SUPER, W, exec, killall -s SIGUSR1 waifuland
# Move waifuland to focused monitor with Super + Shift + W
bind = SUPER SHIFT, W, exec, killall -s SIGUSR2 waifuland
```

## Model Setup

Place Live2D models in your models directory. Each model should be in its own subfolder containing a `.model3.json` file:

```
~/.config/waifuland/models/
├── MyModel/
│   ├── MyModel.model3.json
│   ├── MyModel.moc3
│   ├── textures/
│   └── motions/
└── AnotherModel/
    └── ...
```

The default models directory is `$XDG_CONFIG_HOME/waifuland/models/` (fallback: `~/.config/waifuland/models/`). Override it with the `--models_dir` flag.

## Architecture

```
waifuland/
├── src/                    # Application source code
│   ├── main.cpp            # Entry point, CLI argument parsing
│   ├── LAppWayland.*       # Wayland client setup (display, compositor, EGL, layer-shell)
│   ├── LAppWaylandRegion.* # Input region management (click-through transparency)
│   ├── LAppDelegate.*      # Main app controller, render loop, input handling
│   ├── LAppLive2DManager.* # Model lifecycle management
│   ├── LAppView.*          # View/projection matrices, rendering coordination
│   ├── LAppModel.*         # Individual Live2D model instance
│   └── LAppDefine.*        # Global constants and configuration
├── protocol/               # Wayland protocol XML files
│   ├── xdg-shell.xml
│   └── wlr-layer-shell-unstable-v1.xml
├── thirdParty/             # Third-party dependencies (GLEW, stb)
├── cmake/                  # CMake helper scripts
└── CMakeLists.txt          # Build configuration
```

### Key design decisions

- **No GLFW windowing** — Wayland surfaces are created directly via `wl_compositor` and `zwlr_layer_shell_v1` for overlay behavior that GLFW cannot provide.
- **EGL rendering** — OpenGL context is managed through EGL, bound directly to the Wayland display.
- **Layer-shell overlay** — the application renders as a Wayland layer surface, sitting above normal windows.
- **Input region masking** — only the model's bounding area accepts input; the rest of the surface is fully transparent and click-through.

## Built With

- [Live2D Cubism SDK for Native](https://www.live2d.com/en/sdk/about/) — Live2D model rendering
- [GLEW](https://github.com/nigels-com/glew) — OpenGL extension loading
- [stb_image](https://github.com/nothings/stb) — Image loading
- [wlr-layer-shell](https://wayland.app/protocols/wlr-layer-shell-unstable-v1) — Wayland overlay protocol
- **Wayland** / **EGL** / **OpenGL** — Display and rendering stack

## License

This project includes code adapted from the Live2D Cubism SDK samples, which are subject to the [Live2D Open Software License](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html).

The Live2D Cubism SDK (Core library) is proprietary and must be downloaded separately. See [Live2D SDK License](https://www.live2d.com/en/sdk/license/).
