# projectm-daemon

**A psychadelic music visualizer as your Wayland wallpaper.**

Renders [projectM](https://github.com/projectM-visualizer) presets on a layer-shell surface behind your windows, reacting live to whatever audio is playing. **IPC Controlled**. Runs as a standard window where layer-shell isn't available.

Early release WIP. Stable, but not battle-tested.

## Requirements

A Wayland compositor. Wallpaper mode needs `wlr-layer-shell`. Hyprland, Sway, and other wlroots compositors work. Compositors without it (GNOME) get windowed mode instead.

### Build

- `libprojectm` 4.x, built with GLES. [Installing from git](#projectm-git-instructions) is recommended.
- `libpipewire`
- `cairo`
- wlroots, `wayland-protocols` and `wayland-scanner`
- libgbm, EGL, GLES 3.x
- libjpeg

### Runtime (optional)

- `projectm` preset packs - daemon renders nothing without them
- `playerctl` - track title/artist overlay
- `curl` - album art fetch

## Build and Install

Replace /usr/local/bin with your preferred PATH

```bash
make
sudo cp build/projectm-daemon build/projectm-remote /usr/local/bin
```

## Usage

```
projectm-remote launch        # start the daemon
projectm-remote help          # full command list, served live by the daemon
projectm-remote quit
```

To run as a window only, change the mode config line to `display.mode=windowed`. `auto` prefers layer-shell and falls back to a window.

## ProjectM Git Instructions

For a bleeding-edge version, clone the Git repository and initialize any submodules:

```bash
git clone https://github.com/projectM-visualizer/projectm.git libprojectm
cd libprojectm
git fetch --all --tags
git submodule init
git submodule update
```

### Build libProjectM

Replace `/usr/local` with preferred installation prefix.

```bash
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DENABLE_GLES=ON ..
```

### Install libProjectM
```bash
cmake --build . && sudo cmake --build . --target install
```
