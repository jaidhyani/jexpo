# jexpo

A Hyprland plugin: workspaces 1-9 as a **live** 3x3 grid that you can rearrange with the mouse.

- **Every tile is live.** Video keeps playing and terminals keep scrolling on every workspace,
  not only the one you came from.
- **Drag a window onto another workspace** to move it there.
- **Drag a workspace onto another's place** and the rest slide out of the way as you go. When you
  drop it, the workspaces are **really renumbered**: what was workspace 7 is now workspace 2,
  so `SUPER+2` takes you there. Nothing inside a workspace moves or re-tiles. Bars that follow
  Hyprland's IPC (waybar) update by themselves.
- Click a workspace to go there, or a window to focus it. The view zooms out from where you are
  and back into where you're going.

It started as a fork of [hyprexpo](https://github.com/hyprwm/hyprland-plugins/tree/main/hyprexpo)
and keeps its approach of drawing in place of the monitor's workspace.

**Status:** built and tested on Hyprland **0.53.3** only (Ubuntu 26.04, NVIDIA). Plugins are
compiled against Hyprland's internals, so other versions may need changes; issues and PRs welcome.

## Install

The plugin must be built against the exact Hyprland you run.

**hyprpm**

```sh
hyprpm add https://github.com/jaidhyani/jexpo
hyprpm enable jexpo
```

**make**, with your distro's Hyprland headers installed (`pkg-config --cflags hyprland` must work):

```sh
make
hyprctl plugin load "$PWD/jexpo.so"
```

**Debian/Ubuntu without root:** `./build` downloads `hyprland-dev` for the exact installed version,
plus the -dev packages it needs, with `apt-get download`. It unpacks them into
`~/.cache/jexpo/<version>/`, never installs anything, and compiles `jexpo.so`. It does nothing
when the build is current, so it's cheap to run at every login. After an upgrade, it rebuilds
before you load the plugin:

```ini
exec-once = ~/src/jexpo/build && hyprctl plugin load ~/src/jexpo/jexpo.so
```

## Use

```ini
bind = SUPER, Tab, exec, hyprctl dispatch jexpo:expo toggle
```

(Going through `hyprctl` lets the bind parse before the plugin loads. If you load it with a
`plugin =` line instead, `jexpo:expo, toggle` works directly.)

| in the expo | |
|---|---|
| click a workspace / a window | go there / and focus that window |
| drag a window onto a workspace | move it there |
| drag a workspace by its number badge (or right-drag anywhere, or drag its bare background) | move it to that place; the others shift over; drop renumbers |
| arrows + Enter, or 1-9 | go to a workspace |
| Escape, click off the grid, `SUPER+Tab` again | back to where you were |

Dispatchers:

- `jexpo:expo toggle|close`
- `jexpo:renumber <from> <to>` does the same reorder as a drag, from a script. For example,
  `jexpo:renumber 7 2` moves workspace 7 to 2 and shifts 2-6 to 3-7.

## Config

```ini
plugin {
    jexpo {
        tile_hz = 30            # how often a tile with windows redraws
        empty_hz = 2            # an empty one (wallpaper and bar)
        col_bg = rgb(161a17)    # behind the grid
        col_accent = rgb(c8d8a4)
        font = monospace        # the numbers and the hint line
    }
}
```

All of these can also be set at runtime with `hyprctl keyword plugin:jexpo:<name> <value>`.
If you load the plugin from an `exec-once` (as with `./build` above), a `plugin { jexpo { } }`
block in hyprland.conf is parsed before the plugin exists and reports unknown options, so set
options after the load instead:

```ini
exec-once = ~/src/jexpo/build && hyprctl plugin load ~/src/jexpo/jexpo.so && hyprctl keyword plugin:jexpo:font "FiraCode Nerd Font"
```

## How it works

- **Live tiles.** Each frame it draws workspaces into framebuffers, temporarily making each one
  the monitor's active workspace, with surface feedback on. It also sends frame callbacks, so apps
  on hidden workspaces keep drawing.
- **Frame budget.** The tile being zoomed or dragged redraws every frame. Other tiles redraw at
  most one per frame, the most overdue first, so the work spreads evenly. A timer requests every
  frame while the expo is open, because a frame requested from inside `preRender` is dropped on a
  real DRM output. On a 164 Hz monitor with an RTX 3060 it holds about 6.3 ms per frame.
- **Renumbering** (`renumber.cpp`). Hyprland has no dispatcher for this; a workspace's number is
  its `m_id`, fixed at creation. Windows and monitors hold workspaces by pointer, so changing the
  id in place renumbers a workspace without touching its contents. Along with the id it rewrites:
  - the workspace's name, when the name is just the number
  - the dwindle and master layouts' per-window records
  - windows' last workspace
  - monitors' previous-workspace stacks

  Then it tells IPC listeners each workspace was destroyed under its old number and created under
  its new one.
- **Crash safety.** Every draw call goes through wrappers that skip zero-size boxes and missing
  textures. Hyprland's renderer `RASSERT`s on those, which aborts the whole compositor, and a zoom
  near 0 produces them.

## Limits

- It shows workspaces 1-9 of the focused monitor only. It refuses a reorder that would collide
  with another monitor's workspace number.
- The special workspace isn't drawn in the tiles.
- ext-workspace protocol clients see the new names but keep the old ids until they reconnect.
- A dropped window lands where the target workspace's layout puts it, not under the cursor.

## Testing

`test/stress` is an end-to-end test that runs in about three minutes. It starts a nested Hyprland
under gdb, in a small pinned window in a corner of your session. The nested session runs `waybar`,
`hyprpaper`, a special workspace, and floating, pinned and fullscreen windows. A virtual mouse
(`vpointer.py`, the wlr virtual-pointer protocol over a ~100-line Wayland client) then drives the
expo through:

- open/close at every point of the zoom
- window and workspace drags: the active workspace, an empty one, a fullscreen one, right-button
- clicks, closing mid-drag, the renumber dispatcher
- unloading while open and after use

It checks where every window ends up. On a fault it stops and prints gdb's backtrace, with Ubuntu's
debug symbols fetched through debuginfod. It needs `gdb`, `grim`, `python3` and `ghostty`.

Two things it guards against if you test plugins in a nested Hyprland yourself:

- **The nested instance can leak its environment into your session.** It exports its
  `WAYLAND_DISPLAY` and `HYPRLAND_INSTANCE_SIGNATURE` into your session's systemd and dbus
  environment, even with `HYPRLAND_NO_SD_VARS=1`, via the exec-once path. `test/shim/` blocks
  that.
- **The backtrace can blame the wrong thing.** After a plugin hook has run, any later crash gets
  reported as a crash in Hyprland's hook dispatcher (`CHookSystemManager::emit`). Run under gdb to
  see the real fault.

## License

BSD 3-Clause, the same as hyprexpo, whose code this started from. Written with Claude.
