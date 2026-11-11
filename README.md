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
- [IPC API](#ipc-api)
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
- **JSON configuration** — customize behavior via `config.json` (default model, emotion timeout, additional model dirs, scale/position, window size)
- **IPC control** — Unix domain socket API for scripts and LLM agents: discover and drive expressions (incl. stackable toggles), motions, gaze, pose, any model parameter, lip sync, zoom and position

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

# Run with a custom config file
./build/bin/waifuland --config /path/to/config.json
```

### Configuration

Waifuland reads a JSON config file from `$XDG_CONFIG_HOME/waifuland/config.json` (fallback: `~/.config/waifuland/config.json`). Override the path with `--config <path>`.

Example `config.json`:

```json
{
    "additional_model_dirs": [
        "/home/user/extra-models",
        "/opt/shared-models"
    ],
    "default_model": "MyFavoriteModel",
    "emotion_timeout": 5,
    "model_scale": 1.0,
    "model_x": 0.0,
    "model_y": 0.0,
    "window_width": 1900,
    "window_height": 1000
}
```

| Key | Type | Default | Description |
|---|---|---|---|
| `additional_model_dirs` | string[] | `[]` | Extra directories to scan for models (in addition to the default models dir) |
| `default_model` | string | `""` | Name of the model subfolder to load first on startup |
| `emotion_timeout` | float | `5` | Seconds before expression reverts to default. Set to `-1` to never revert |
| `model_scale` | float | `1.0` | Initial model scale |
| `model_x` | float | `0.0` | Initial model X offset |
| `model_y` | float | `0.0` | Initial model Y offset |
| `window_width` | int | `1900` | Render target width in pixels |
| `window_height` | int | `1000` | Render target height in pixels |

All fields are optional. Missing fields use their default values. If the config file doesn't exist, all defaults are used.

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

## IPC API

Waifuland exposes a Unix domain socket so scripts, programs and LLM agents can control the model.

- **Socket path:** `$XDG_RUNTIME_DIR/waifuland.sock` (fallback: `/tmp/waifuland.sock`)
- **Protocol:** newline-delimited JSON. Send `{"command":"<name>", ...}\n`, receive one line `{"ok":true, ...}\n`. Errors are `{"ok":false,"error":"..."}`.
- **Discovery:** `list_commands` lists every command with its arguments; `describe_model` describes what the current model supports.

### CLI Client

`waifuland-ctl` (project root) builds the JSON for you:

```bash
./waifuland-ctl [-c] <command> [--key value | --key=value | --flag ...]
./waifuland-ctl [-c] --json '<request JSON>'
```

- Values are sent as JSON numbers, `true`/`false`/`null`, raw JSON when they start with `{` or `[`, and as strings otherwise. A `--flag` without a value means `true`.
- `-c` prints the response on one line (default: pretty-printed, non-ASCII kept readable).
- Exit status: `0` ok, `2` Waifuland returned an error, `1` usage/connection error.
- Needs `socat` or `python3`.

### Control model

Everything an agent can change, and how long it lasts:

| Layer | Commands | Lifetime |
|---|---|---|
| Main expression | `set_expression` | One at a time; reverts after `duration` (default: config `emotion_timeout`) |
| Toggled expressions | `toggle_expression`, `clear_expressions` | Stack with each other and the main expression; optional `duration` |
| Motions | `do_motion`, `stop_motion` | Play once; the idle motion resumes afterwards. Parameters a motion leaves behind stay until changed (used for held poses) |
| Parameter overrides | `set_pose`, `set_parameters`, `release_pose`, `release_parameters` | Until released, or for `duration` seconds; blend in/out over `fade` seconds |
| Gaze | `set_look` | Until `reset`, or for `duration` seconds; then the eyes follow the cursor again |
| Lip sync | `set_mouth_y` | Until `reset`, or for `duration` seconds |

`reset_model` stops motions, clears expressions, overrides, gaze and mouth, and puts every parameter back to its default.

Parameter overrides are applied before physics, so hair and clothes react to them. Overwrite-blend overrides are applied again at the end of the frame when something later overwrote the parameter (physics outputs, lip sync), so they always win. Re-setting an active override moves smoothly to the new value over `fade`.

### Commands

| Command | Arguments | Description |
|---|---|---|
| `list_commands` | — | All commands with arguments |
| `describe_model` | — | Expressions, motions, pose controls (with what they map to on this model), parameter groups, hit areas, current state |
| `get_status` | — | Window/model status plus current expression, toggled expressions, motion state, overrides, gaze and mouth overrides |
| `get_available_models` | — | Model names |
| `get_current_model` | — | Current model name and index |
| `set_model` | `name` or `index` | Switch model |
| `next_model` / `prev_model` | — | Cycle models |
| `get_expressions` | `detail?` | `expressions`, `active` (toggled), `current` (main); `detail=true` adds the parameters each expression changes |
| `set_expression` | `id`, `duration?` | Main expression. `duration` in seconds, `-1` = never revert |
| `toggle_expression` | `id`, `active?`, `duration?` | Flip a stackable expression, or set it with `active`; `duration` turns it off again after N seconds |
| `clear_expressions` | — | Turn off all expressions |
| `get_motions` | — | Motions (`group`, `index`, `file`, `duration`, `fade_in`, `fade_out`) and the current motion `state` |
| `do_motion` | `group?`, `index?`, `priority?` | Play a motion. No `index` = random in the group, no `group` = random `TapBody` motion. Priority: 1 idle, 2 normal (default), 3 force. Fails if an equal or higher priority motion is playing |
| `stop_motion` | — | Stop motions; idle resumes |
| `get_parameters` | `filter?`, `ids?` | Parameters with display `name` and `group` (from `.cdi3.json`), `min`/`max`/`default`, current `value`, and `physics_output`/`physics_source`. `filter` matches id, name or group; `ids` is a list or comma-separated string |
| `set_parameters` | `params{id:value}`, `duration?`, `fade?`, `blend?` | Drive any parameters. `blend`: `overwrite` (default), `add`, `multiply`. `fade` default 0.3s. All-or-nothing: an unknown id rejects the request |
| `release_parameters` | `ids?`, `fade?` | Release overrides (all when `ids` is omitted) |
| `set_pose` | `pose{alias:value}` or alias keys, `duration?`, `fade?` | Model-independent controls, see below. Returns the parameters each alias drove and `unsupported` aliases |
| `release_pose` | `keys?`, `fade?` | Release pose aliases (all overrides when `keys` is omitted) |
| `set_look` | `x`, `y`, `duration?` or `reset` | Gaze direction, -1..1 (`+x` = viewer's right, `+y` = up). Moves eyes, head and body like cursor tracking |
| `set_mouth_y` | `value`, `duration?` or `reset` | Lip-sync mouth opening 0..1, for streaming audio-driven lip sync |
| `reset_model` | — | Back to a clean default state |
| `set_model_zoom` | `value` | Scale 0.1..10 |
| `set_model_position` | `x?`, `y?` | Model offset |
| `get_model_position` | — | Offset and scale |
| `toggle_hidden` | — | Toggle visibility |
| `switch_skin` | — | Cycle skins (models whose motion groups are outfits) |
| `batch` | `commands[]` | Run several commands in the same frame; returns `results[]` and `ok` = all succeeded |

### Pose controls

`set_pose` maps normalized values onto the standard Cubism parameters, using each parameter's own range on the current model. If physics overwrites a head/body angle from a single input (common in VTube Studio models, e.g. `AngleX → ParamAngleX`), the input is driven instead so physics stays natural. `describe_model` lists which aliases the current model supports.

| Alias | Range | Meaning |
|---|---|---|
| `head_x`, `head_y`, `head_z` | -1..1 | Head turn (+ viewer's right), tilt (+ up), roll (+ top of head toward viewer's right) |
| `body_x`, `body_y`, `body_z` | -1..1 | Body turn, lean, roll |
| `eye_x`, `eye_y` | -1..1 | Eyeballs (+ viewer's right / up) |
| `eye_open`, `eye_open_l`, `eye_open_r` | 0..2 | 0 closed, 1 normal, 2 widest |
| `eye_smile` | 0..1 | Smiling eyes |
| `brow_y`, `brow_angle`, `brow_form` | -1..1 | Eyebrow height, angle, shape |
| `mouth_open` | 0..1 | Mouth opening |
| `mouth_form` | -1..1 | Mouth shape (- frown, + smile) |
| `cheek` | 0..1 | Blush |
| `arm_l`, `arm_r`, `arm_l_b`, `arm_r_b`, `hand_l`, `hand_r`, `shoulder_y` | -1..1 | Standard arm/hand/shoulder parameters (many models use custom ones instead: find them with `get_parameters`) |

### Examples

```bash
# Learn what the model can do
./waifuland-ctl describe_model
./waifuland-ctl get_expressions --detail
./waifuland-ctl get_parameters --filter mouth

# Expressions
./waifuland-ctl set_expression --id happy --duration 5
./waifuland-ctl toggle_expression --id glasses --active true
./waifuland-ctl toggle_expression --id blush --duration 3

# Motions
./waifuland-ctl do_motion --group TapBody --index 0
./waifuland-ctl stop_motion

# Temporary pose: turn head, smile, open mouth for 3 seconds
./waifuland-ctl set_pose --head_x 0.6 --mouth_form 1 --mouth_open 0.5 --duration 3

# Any parameter, held until released
./waifuland-ctl set_parameters --params '{"ParamCheek":1,"ParamArmLA":-5}' --fade 0.5
./waifuland-ctl release_parameters

# Look at the viewer for 2 seconds
./waifuland-ctl set_look --x 0 --y 0 --duration 2

# Several changes in the same frame
./waifuland-ctl --json '{"command":"batch","commands":[
  {"command":"set_expression","id":"happy"},
  {"command":"set_pose","pose":{"head_y":0.3},"duration":2},
  {"command":"do_motion","group":"TapBody"}]}'
```

### Raw Socket Usage

```bash
# Using socat
echo '{"command":"get_status"}' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/waifuland.sock
```

```python
import json, os, socket

def waifuland(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "waifuland.sock"))
    s.sendall((json.dumps(cmd) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):  # responses can be large (get_parameters)
        buf += s.recv(65536)
    s.close()
    return json.loads(buf)

print(waifuland({"command": "set_pose", "pose": {"head_x": 0.5}, "duration": 2}))
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
│   ├── LAppConfig.*        # JSON config file reader (singleton)
│   ├── LAppWayland.*       # Wayland client setup (display, compositor, EGL, layer-shell)
│   ├── LAppWaylandRegion.* # Input region management (click-through transparency)
│   ├── LAppDelegate.*      # Main app controller, render loop, input handling
│   ├── LAppLive2DManager.* # Model lifecycle management
│   ├── LAppView.*          # View/projection matrices, rendering coordination
│   ├── LAppModel.*         # Individual Live2D model instance
│   ├── LAppIPC.*           # IPC socket server for external control
│   ├── LAppJson.*          # JSON parser/writer used by the IPC protocol
│   └── LAppDefine.*        # Global constants and configuration
├── protocol/               # Wayland protocol XML files
│   ├── xdg-shell.xml
│   ├── xdg-output-unstable-v1.xml
│   ├── fractional-scale-v1.xml
│   ├── viewporter.xml
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
