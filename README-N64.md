# Elite - The New Kind for Nintendo 64

A port of Christian Pinder's *Elite - The New Kind* to the N64 (libdragon),
with a dashboard and icon bar rebuilt from the NES version of Elite.

## Screen layout

* **Space views** - a 512x320 view with the NES-style dashboard beneath it.
* **Every other screen** - the full original 512x384 text screen with just the
  icon bar underneath (as on the NES, the dashboard is hidden).

The icon bar changes with the situation: docked, in flight, or on a chart.
Buttons that cannot be used right now (no E.C.M. fitted, no destination
chosen, and so on) are shown blank.

## Controls

### In the space views

| Control | Action |
|---|---|
| Analog stick | Pitch and roll (push forward to dive) |
| C-buttons | Pitch and roll, digital |
| D-pad up / down | Speed up / slow down |
| Z (hold) | Slow down |
| A (hold) | Fire laser |
| D-pad left / right | Move the icon bar pointer |
| B | Use the selected icon |
| R | Next view: front, rear, left, right |
| L | Front view |
| Start | Pause menu (lists every screen and function) |

### On every other screen

| Control | Action |
|---|---|
| D-pad or stick | Move through lists, buy (right) / sell (left), move the chart cursor |
| A | Confirm / buy / select |
| B | Cancel. In flight: back to the front view |
| C-left / C-right | Move the icon bar pointer |
| C-down | Use the selected icon |
| Start | Pause menu |

### Shortcuts

Hold the shoulder button(s), then press:

| Shortcut | Action | Shortcut | Action |
|---|---|---|---|
| R + D-up | Galactic chart | L + D-up | Commander status |
| R + D-down | Short range chart | L + D-down | Inventory |
| R + D-left | Data on system | L + D-left | Equip ship (docked) |
| R + D-right | Market prices | L + D-right | Options |
| L+R + A | Hyperspace | L+R + B | Galactic hyperspace |
| L+R + C-up | Docking computer | L+R + C-down | In-system jump |
| L+R + C-left | Energy bomb | L+R + C-right | Escape pod |
| L+R + D-up | Chart distance / docking computer off | L+R + D-down | Scanner zoom |
| L+R + D-left | Chart cursor back to current system | L+R + Z | Find planet (on a chart) |

L and R on their own act when released, and do nothing while docked.

### Finding a planet

On a chart choose the **?** icon (or L+R+Z, or Find Planet in the pause menu).
D-pad up/down picks a letter, right moves on to the next, A searches,
B deletes a letter (or cancels when the name is empty).

## Commander name

While docked, the **AAA** button on the icon bar changes the commander's name
(up to 7 letters, entered the same way as a planet name). It is saved with
the commander.

## Rumble Pak

With a Rumble Pak in controller 1 you feel incoming laser fire, collisions
and the loss of your ship.

## Saving

One commander is kept in the cartridge EEPROM (Save Commander on the Options
screen or in the pause menu, while docked). Game Settings are kept there too.
The title screen offers **A: Load Game** when a save exists.

## Enhanced graphics

Game Settings > Graphics has three modes: **Enhanced** (the default), Solid
and Wireframe. Enhanced draws the ships with the N64's 3D hardware through
tiny3d: the original Elite ship shapes and face colours, lit from the
system's sun and depth-buffered. Solid and Wireframe are the previous
software renderers, unchanged.

Ships listed in `ships/` (so far the Cobra Mk III) also get painted surface
detail. The geometry and each face's colour are exactly the originals; the
paintwork on top is laid out over the whole ship: irregular hull panels, a
cockpit canopy at the nose, wing stripes and a roundel, hazard markings and
scorching by the engines, cargo doors on the belly, weathering, and an exhaust
glow on engine faces.

An **Expansion Pak is required**.

### Detailing a ship

    python3 tools/detail_ship.py cobra3 cobra3a_point
    python3 tools/gen_ship_textures.py          # rewrites n64_shiptex.c

`detail_ship.py` reads the ship straight from `shipdata.c` / `shipface.c` and
writes its textures and layout to `ships/`. Textures are 16-colour tiles of at
most 64x64 (what fits the N64's texture memory at once); a large face is
covered by up to 2x2 tiles so the detail stays sharp, and mirror-image faces
share textures. List the game ship types that use it in `SHIP_TYPES` in
`tools/gen_ship_textures.py`. `tools/ship_export.py` renders pictures of the
original ships and exports their meshes.

## How the N64 hardware is used

* The space views, title screens and cut-scenes are drawn straight into the
  display buffer; only the static text screens go through a frame copy.
* The RDP (graphics chip) wipes the view at the start of each frame while the
  CPU reads the controller and runs the simulation, and fills the banded
  planet as one textured rectangle per screen line.
* In Enhanced mode ships are transformed and lit by the RSP and drawn by the
  RDP with a depth buffer (tiny3d). Stars, text and the dashboard are drawn
  by the CPU.
* Sound effects and music are mixed by libdragon's RSP mixer.

## Building

Needs the libdragon toolchain (`N64_INST` set) and tiny3d
(https://github.com/HailToDodongo/tiny3d) built with it. The Makefile looks
for tiny3d in `$(N64_INST)/tiny3d`; point `TINY3D_DIR` elsewhere if needed:

    make -f Makefile.n64 [TINY3D_DIR=/path/to/tiny3d]

Add `EXTRA_CFLAGS=-DN64_SHOW_FPS` for a frames-per-second readout.

* `scanner.bmp` and `iconbar.bmp` are generated by `build_dashboard.py` from the
  documented NES Elite source (https://github.com/markmoxon/elite-source-code-nes):
  `NES_SRC=/path/to/elite-source-code-nes/1-source-files python3 build_dashboard.py`
* `music/*.wav` are rendered from `data/*.mid` by `tools/render_music.sh`
  (fluidsynth); the build itself only needs the WAV files.
* `n64_spritedata.c` is generated from `data/sprites` by `tools/convert_assets.py`.
