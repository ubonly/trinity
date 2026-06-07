# Trinity WM 

Trinity WM is a lightweight, dynamic tiling window manager for X11 written in C. Inspired by classic window managers like `dwm` and `sxwm`, Trinity WM brings a fast, minimalistic, and modern tiling environment to Xorg with runtime configuration hot-reloading.

## Features

- ⚡ **Lightweight & Fast**: Built directly on top of Xlib. Extremely small memory footprint.
- 📐 **Dynamic Tiling Layout**: Master/Stack layout supporting window resizing, tag switching, and floating toggles.
- ⚙️ **Runtime Configuration**: No need to recompile the WM to change binds or colors! Reads binds from `~/.config/trinitywm/trinitywm.conf` at startup.
- 🔄 **Hot-Reloading**: Automatically detects config modifications using `inotify` and applies them instantly on save.
- 🎨 **Modern Aesthetics**: Supports active/inactive border coloring, rounded layout gaps, and coordinates perfectly with composite managers like `picom`.
- 🔍 **EWMH Compliant**: Seamlessly integrates with system fetch utilities (like `fastfetch` or `neofetch`) and desktop status bars.

---

## Installation

### 1. Dependencies

Ensure you have Xlib headers, `make`, and `gcc` installed.

**Arch Linux:**
```bash
sudo pacman -S libx11 base-devel
```

**Ubuntu / Debian:**
```bash
sudo apt install libx11-dev build-essential
```

### 2. Build and Install

Clone this repository and compile:

```bash
git clone https://github.com/ubonly/trinity.git
cd trinity
make
sudo make install
```

This compiles the source and installs the `twm` binary to `/usr/local/bin/twm`.

---

## How to Run

Add `exec twm` to the end of your `~/.xinitrc` file:

```bash
# ~/.xinitrc
exec twm
```

Then run `startx` to start your session.

---

## Configuration

On the first run, Trinity WM will automatically generate a default configuration file at:
`~/.config/trinitywm/trinitywm.conf`

You can customize the keybindings, window rules, colors, and layout directly in this file. Saving the file triggers an automatic hot-reload immediately!

### Default Keybindings

- **Super + Q**: Open terminal (Default: `alacritty`)
- **Super + D**: Open application menu (Default: `dmenu_run`)
- **Super + C**: Close active window
- **Super + J / K**: Focus next/previous window
- **Super + H / L**: Shrink/expand master area size
- **Super + Space**: Toggle floating status for window
- **Super + 1-9**: Switch to workspace (tags 1 to 9)
- **Super + Shift + 1-9**: Move window to workspace
- **Super + Shift + Q**: Exit Trinity WM
