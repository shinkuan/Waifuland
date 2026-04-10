# Waifuland

A Live2D desktop mascot application built with Tauri 2 + PIXI.js. The model is rendered on a transparent, borderless, always-on-top window, and its eyes follow your cursor across all monitors (Hyprland).

## Prerequisites

- [Node.js](https://nodejs.org/) (v18+)
- [Rust](https://rustup.rs/) (stable)
- [Tauri 2 CLI](https://v2.tauri.app/start/prerequisites/)
- **Hyprland** window manager (cursor tracking uses `hyprctl cursorpos`)

## Setup

```bash
npm install
```

## Development

```bash
npm run tauri dev
```

## Production Build

```bash
npm run tauri build
```

The built binary will be in `src-tauri/target/release/`.

## Adding Models

Place Live2D Cubism 4 models under the `models/` directory. Each model should be in its own subdirectory containing a `.model3.json` file:

```
models/
  my-model/
    my-model.model3.json
    my-model.moc3
    textures/
      ...
```

Restart the application after adding new models. Right-click to switch between loaded models.

## Usage

### Window Controls

| Action | Description |
|--------|-------------|
| **Left-click + drag** | Move the window |
| **Scroll wheel up** | Enlarge the window (and model) |
| **Scroll wheel down** | Shrink the window (and model) |
| **Right-click** | Open context menu |

### Context Menu

- **Model list** - Click a model name to switch to it
- **Always on Top: ON/OFF** - Toggle whether the window stays above other windows
- **Quit** - Close the application

### Window Size

The window starts at 600x800 pixels. Use the scroll wheel to resize (range: 200-2000 px per side). The model automatically scales to fill 90% of the window height.

### Eye Tracking

The model's gaze follows your mouse cursor. This works across all monitors on Hyprland by reading the global cursor position via `hyprctl cursorpos`.

## Project Structure

```
Waifuland/
  index.html            # HTML entry point
  src/
    main.js             # Frontend logic (PIXI, Live2D, window controls)
    style.css           # Styles (context menu, canvas)
  src-tauri/
    src/lib.rs          # Rust backend (model listing, file serving)
    tauri.conf.json     # Tauri window & build configuration
    capabilities/       # Permission definitions
  models/               # Live2D model files
  public/lib/           # Bundled PIXI.js + Live2D Cubism libraries
```

## Configuration

### Window defaults

Edit `src-tauri/tauri.conf.json` to change default window size, transparency, etc.:

```json
"windows": [{
  "width": 600,
  "height": 800,
  "decorations": false,
  "transparent": true,
  "alwaysOnTop": true
}]
```

### Resize limits

In `src/main.js`, the scroll-wheel resize range is controlled by:

```javascript
const newWidth = Math.max(200, Math.min(2000, ...));
```

Change `200` (minimum) and `2000` (maximum) to adjust the resize bounds.
