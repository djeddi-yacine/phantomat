# Phantomat

A zoomable, infinite-canvas window manager for Hyprland. Every window lives
on one endless plane instead of in workspaces. Press a key and the view pulls
back through a curved lens so you can see everything at once, then type a few
letters and fly straight to the window you want.

The name comes from Stanisław Lem's *Summa Technologiae*, where a phantomat
is a machine that builds a whole world around the person inside it.

<!-- Demo video: drag the recording into this spot in GitHub's editor. -->

Made on [Omarchy](https://omarchy.org) with Hyprland 0.56. Built on
[hyprland-scroll-overview](https://github.com/yayuuu/hyprland-scroll-overview)
by yayuuu (see [Credits](#credits)).

## What it does

- **One canvas for everything.** Windows float freely on a single 2D plane
  instead of in workspaces. Pan, zoom and place windows anywhere.
- **Your screens are one desk.** Side by side on the canvas as they stand on
  your desk, moving together: a window can sit across the seam, and dragging
  it from one screen to the other is one continuous move.
- **Workspace-bound Canvas (opt-in).** Set `canvas.workspace_bound=true`,
  `canvas.linked_screens=false`, and `canvas.places=false` to give every real
  Hyprland workspace its own window set, camera, and remembered placement.
  Switching workspaces keeps the outgoing Canvas renderer alive through
  Hyprland's workspace transition, without retiling, resizing, or moving its
  clients. The shell uses slide workspace transitions, and Canvas clients
  preserve Hyprland's animated workspace opacity in both directions. The moving
  plane is the monitor-sized view through the saved camera, not the infinite
  Canvas: surfaces, borders and shadows are clipped to that translated view.
  Off-camera clients cannot slide into a different or empty native workspace.
  Native clients, pinned windows, special workspaces and layers keep their
  native rendering and damage tracking outside that clip. Returning
  resumes Canvas at its saved camera with the navigation HUD closed by default.
  Set `canvas.remember_overview=true` to instead restore the workspace's
  previous overview zoom, camera, search and selected live client. This is
  compositor-session state, independent of `remember_layout`; explicit Canvas
  disable and plugin unload clear it. Inactive workspaces never retain HUD input
  ownership. Fast reversals reuse the retained renderer rather than allocating
  another one.
  Once the native transition settles, inactive renderers are released. Explicitly
  disabling Canvas or unloading the plugin restores the saved native layout.
  No windows are reassigned to follow a monitor.
  Overview chrome fades with the native workspace's visible fraction and alpha,
  using the existing slide rather than a second animation. Selected outlines
  move and clip with their clients. The outgoing renderer keeps only its last
  visual HUD frame while live search/input ownership ends immediately; incoming
  and outgoing overlays compose in the native pass, not one shared shader slot.
- **Type to find.** `SUPER + CTRL + G` zooms out with a search bar already
  listening. Type part of a title or app name and the camera glides to the
  best match; `Enter` lands on it at full size, `Esc` takes you back.
- **Keyboard first.** Jump to the nearest window in any direction, nudge
  windows around, bring a window to where you are, frame one or fit them all,
  tidy everything up by app, undo and redo.
- **Choose a Hyprland layout.** `SUPER + F1` dwindle, `F2` scrolling, `F3`
  master and `F4` monocle on the current regular workspace.
- **Recent windows.** `ALT + TAB` flips between windows most recently used
  first; hold `ALT` to see the list.
- **Fill or go fullscreen.** `SUPER + T` makes a window fill its screen and puts
  it back. Fullscreen (`SUPER + F`, a video, a game) takes over just the screen
  the window is on; the other screens keep the canvas.
- **All your apps.** Wayland and X11 apps alike, games under Wine and Proton
  included: menus, drags, fullscreen and full frame rate work as on a normal
  desktop.
- **It remembers.** Windows return to their spots after a restart.
- **Tune it live.** `CTRL + ,` in the zoomed-out view opens a tuner for the
  lens, blur, grid, parallax, HUD size, type, colors and more, and you see
  each change as you make it.
- **Looks the part.** A barrel lens with optional edge blur, vignette and
  color fringe, a parallax wallpaper, a dotted grid, a minimap and a
  monospace HUD in the accent color of your choice.

## Install

You need Hyprland 0.56 or newer with its Lua config (`~/.config/hypr/hyprland.lua`,
which Omarchy uses), GCC 15 or newer, and Hyprland's development files. On
Arch or Omarchy:

```sh
sudo pacman -S --needed base-devel git hyprland hyprgraphics pango lua
git clone https://github.com/kaolti/phantomat.git
cd phantomat
scripts/install.sh
```

The installer builds the plugin, puts it in `~/.local/share/spatial-overview/`,
copies the default settings to `~/.config/hypr/spatialoverview.lua` (it never
overwrites yours), adds two marked lines to your `hyprland.lua` (after making a
backup), and loads it right away. Then press `SUPER + CTRL + G`.

Phantomat used to be called Spatial Overview, and inside it still is: the
settings file, the `hl.plugin.spatialoverview` actions and the
`hyprctl spatialoverview` command keep that name, so configs from before the
rename keep working.

**After a Hyprland update**, the plugin has to be rebuilt for the new version.
Hyprland shows a notification when that is the case; run the installer again:

```sh
cd phantomat && git pull && scripts/install.sh
```

Updating never touches your `~/.config/hypr/spatialoverview.lua`, so keys added
in a new version are not in it yet: compare it with
[examples/spatialoverview.lua](examples/spatialoverview.lua) and copy what you
want (or move yours aside and run the installer for a fresh one).

**To uninstall**, run `scripts/uninstall.sh`. Windows go back to your normal
layout, the lines come out of `hyprland.lua`, and the plugin is deleted. Your
settings stay, in case you come back; `scripts/uninstall.sh --purge` deletes
them too.

## Keys

### Anywhere

| Keys | Action |
| --- | --- |
| `SUPER + CTRL + G` | Zoom out to the canvas (and back). |
| `ALT + TAB`, `ALT + SHIFT + TAB` | Recent windows. A tap flips to the previous one; hold `ALT` for the list, release to go. |
| `SUPER` + arrows | Focus the nearest window in that direction; the camera follows. |
| `SUPER + SHIFT` + arrows | Nudge the focused window one grid step; hold to keep moving. |
| `SUPER + T`, `SUPER + ALT + F` | Make the focused window fill its screen, with the usual gaps; again puts it back. |
| `SUPER + F`, or an app going fullscreen | Fullscreen on the screen the window is on; the other screens keep the canvas. `SUPER + CTRL + G` takes the screen back, going back to the window makes it fullscreen again. |
| `SUPER + O` | Pin the window to the screen: it stays put while the canvas moves; again puts it back on the canvas. |
| `SUPER + 1` … `0`, `SUPER + TAB` and the other workspace keys | Nothing, on the canvas (with `canvas.places`, experimental: places on the canvas). |
| `SUPER + J`, `P`, `L`, `Home`, `G`, `SHIFT + ALT + SUPER` + arrows | Tiling and grouping keys: nothing, on the canvas (every window floats). |
| Middle-drag | Pan the canvas. |
| `CTRL` + wheel, pinch | Zoom. |
| `SUPER` + left-drag, right-drag | Move, resize a window. |

### In the zoomed-out canvas

| Keys | Action |
| --- | --- |
| type | Search window titles and app names. Every word must appear as typed, letters together (`fas` finds `fastfetch`); case and accents are ignored. The camera and focus follow the best match. |
| `↑` `↓`, `Tab` `SHIFT + Tab` | Move through the results. With no search, `Tab` walks recent windows. |
| `CTRL + 1` … `CTRL + 9` | Go straight to that result. |
| `Enter` | Go to the selected window at full size. |
| `SHIFT + Enter` | Bring the selected window to where you are, then go there. |
| `Esc` | Clear the search, then go back to where you started. |
| `←` `→` `↑` `↓` | Select the nearest window in that direction. |
| `SHIFT` + arrows | Nudge the selected window. |
| `CTRL` + arrows | Pan. |
| `CTRL + =`, `CTRL + -` | Zoom in, out. |
| `CTRL + 0` | Fit every window (every match while searching). |
| `CTRL + F` | Frame the selected window. |
| `CTRL + A` | Tidy all windows up on the grid, by app. |
| `CTRL + Z`, `CTRL + SHIFT + Z` | Undo, redo moves and tidying. |
| `CTRL + ,` | Tune the look. |
| `F1` | Every key, on screen. |

With the mouse: click a window to go to it, drag a window to move it, drag the
empty canvas to pan, scroll to zoom, click the minimap to jump.

## Tuning

`CTRL + ,` in the zoomed-out canvas turns the search bar into a list of
settings. Type to filter (`blur`, `grid dot`, `accent`), `↑` `↓` to pick, `←`
`→` to adjust while you watch (`SHIFT` for fine steps, `CTRL` for big ones),
`Delete` to undo a change, `Esc` when done. The footer explains the selected
setting.

Changes are saved as you make them to `~/.config/hypr/spatialoverview-tuning.lua`,
which is applied after `spatialoverview.lua`, so a tuned value wins until you
delete its line there.

Everything can also be set in `~/.config/hypr/spatialoverview.lua` (then
`hyprctl reload`). The main settings, all under `plugin.spatialoverview`:

| Setting | Effect |
| --- | --- |
| `canvas.desktop_mode` | Enable the shared infinite-window desktop |
| `canvas.workspace_bound` | Scope Canvas to the active real workspace; requires `desktop_mode=true`, `linked_screens=false`, and `places=false`. Canvas mode stays at 100% until the separate HUD toggle. |
| `canvas.linked_screens` | Screens show adjacent parts of the canvas and move together (default); off: each screen is its own camera |
| `canvas.places` | Experimental, off by default: the workspace keys go to places on the canvas and take windows there |
| `canvas.initial_zoom` | Camera zoom when desktop mode opens |
| `canvas.min_zoom` / `max_zoom` | Continuous camera zoom limits |
| `canvas.zoom_step` | Ctrl-wheel zoom strength |
| `canvas.space_pan` | Optional Space + left-drag panning; disabled by default so Space always reaches applications |
| `canvas.direct_input` | Forward input into transformed Wayland windows |
| `canvas.hover_focus` | Activate a transformed window when the pointer enters it |
| `canvas.persistent` | Keep the canvas renderer active at 100% zoom instead of returning to workspace rendering |
| `canvas.minimap_*` | Configure the navigation-mode minimap size, margin, and opacity |
| `canvas.minimap_rounding` | Corner radius in logical pixels (default 14); zero squares the panel, markers, viewport and arrange control in both flat and lens rendering |
| `canvas.arrange_context_grouping` | Prefer shared local title/app context before falling back to app categories |
| `canvas.arrange_size_similarity` | Relative threshold for treating window sizes as equivalent |
| `canvas.arrange_resize_limit` | Maximum relative resize applied to any window by smart arrangement |
| `canvas.auto_float` | Detach existing and new app windows from tiling |
| `canvas.auto_place` | Place newly managed windows near the active camera |
| `canvas.placement_gap` | Collision gap used by automatic placement |
| `input.pan_sensitivity` | Middle-drag camera sensitivity (and optional Space-drag sensitivity) |
| `input.drag_threshold` | Pixels before a click becomes a drag |
| `animation.enabled` | Enables overview camera animation |
| `animation.speed` | Camera animation speed |
| `animation.bezier` | Name of the Hyprland curve used for camera motion |
| `distortion.enabled` | The curved lens at all |
| `distortion.strength` | Signed lens curvature; `0` is flat |
| `distortion.shader_path` | A lens shader of your own; empty uses the one built into the plugin |
| `distortion.edge_scale` | Overscan that keeps curved corners filled |
| `distortion.feather` | Width of the soft screen-edge fade |
| `distortion.transition_power` | When the lens eases in during the zoom animation (1 follows the zoom) |
| `canvas.grid_*` | Shared snap/render spacing, width, opacity, line/dot style, and dot diameter |
| `canvas.background_dim` | Dark overlay behind the grid and windows in navigation mode |
| `canvas.snap_enabled` / `snap_size` | Snap free window placement to the canvas grid, and its step in pixels |
| `canvas.remember_layout` | Remember window positions and cameras across restarts and reopenings |
| `canvas.remember_overview` | Restore workspace-bound overview zoom, camera, search and selection on return. Session only; default `false`. Fresh Canvas activation still starts with the HUD closed. |
| `navigator.enabled` | Type-to-search palette in the zoomed-out canvas |
| `navigator.auto_open` | Open that palette automatically on Canvas entry (default: `true`) |
| `navigator.labels` | Window titles on the map |
| `navigator.dim_unmatched` | How far windows that do not match the search recede |
| `navigator.pointer` | Click lands, drag moves or pans, wheel zooms; off keeps direct app input while zoomed out |
| `navigator.accent` | HUD accent for the selected row, caret, key names and outlines: `#rrggbb`, a preset name (`orange`, `amber`, `yellow`, `lime`, `green`, `mint`, `teal`, `cyan`, `sky`, `blue`, `indigo`, `violet`, `purple`, `magenta`, `pink`, `red`, `white`), or `auto` to follow the active border |
| `navigator.colors.panel` / `card` / `edge` | Native Hyprland colors for HUD surfaces and outlines; the minimap and canvas dim overlays share the panel color, minimap outlines use edge |
| `navigator.colors.text` / `text_dim` / `text_faint` | Native Hyprland colors for primary, secondary and tertiary text; the grid and inactive minimap markers use `text_dim` |
| `navigator.mono_font` | The HUD's typeface (monospace, set in uppercase) |
| `wallpaper` / `blur` / `blur_strength` | Sharp-at-rest canvas backdrop and navigation-mode blur blend |
| `distortion.edge_blur` / `edge_blur_start` | Lens blur toward the screen edges, and where it begins (0 center, 1 corners) |
| `distortion.vignette` | Darkening toward the edges |
| `distortion.chromatic` | Red/blue fringe toward the edges |
| `parallax.enabled` / `strength` | The wallpaper follows the camera at this fraction of the windows' speed |
| `parallax.depth` | How much the wallpaper shrinks as you zoom out (0 keeps its size) |
| `parallax.desktop` | Also apply parallax when panning the full-size desktop |
| `navigator.hud_scale` | Size of the whole HUD |
| `navigator.width` / `top` / `rows` / `row_height` / `rounding` | Palette geometry |
| `navigator.panel_opacity` | Palette background opacity (lower is glassier) |
| `navigator.uppercase` / `letter_spacing` | Capitals or text as written, and tracking |
| `navigator.query_size` / `title_size` / `detail_size` / `corner_size` / `label_size` | Type sizes |
| `navigator.corner_labels` | Readouts in the screen corners |
| `navigator.search_height` / `search_border` / `search_border_opacity` / `search_border_color` / `search_glow` | The search field |
| `navigator.tuning_file` | Where the live tuner saves (default `$XDG_CONFIG_HOME/hypr/spatialoverview-tuning.lua`) |

Set both `navigator.rounding` and `canvas.minimap_rounding` to `0` for square
HUD surfaces, labels, badges, minimap markers and controls. Color roles accept
native `rgb(...)` and `rgba(...)` values. Panel/card alpha combines with
`navigator.panel_opacity`; minimap panel alpha additionally combines with
`canvas.minimap_opacity`. Changing any color role invalidates cached HUD
textures, including when the accent stays unchanged.

Lua configs can replace the complete per-output color map after `hl.config`:

```lua
hl.plugin.spatialoverview.output_colors({
  [output_name] = {
    accent = "rgb(50ece0)",
    panel = "rgb(0e0e0f)", card = "rgb(1b1b1c)", edge = "rgba(45697aaa)",
    text = "rgb(ededed)", text_dim = "rgb(8f8f91)", text_faint = "rgb(5c5c5e)",
  },
})
```

`output_name` is an exact monitor name supplied by your configuration. All seven
roles are required and use native Hyprland color syntax. A bad entry rejects the
whole update. The map replaces previous assignments, including on config reload;
`output_colors({})` removes them all. Unassigned outputs use `navigator.accent`
and `navigator.colors` as their configured default. HUD, minimap, grid, labels
and overlays resolve colors from the monitor being rendered, not the focused
monitor. Texture and hit-test caches are output-scoped. `search_border_color =
"accent"` follows that output's accent; an explicit color remains shared.

Window selection, search-match and hover outlines follow each window's native
rounding (including window rules); a square window keeps square outlines even
when those outlines are expanded away from its edges.

`navigator.accent` takes `#rrggbb`, a preset name (`orange`, `amber`, `yellow`,
`lime`, `green`, `mint`, `teal`, `cyan`, `sky`, `blue`, `indigo`, `violet`,
`purple`, `magenta`, `pink`, `red`, `white`), or `auto` for your theme's border
color.

For scripts and bindings, `hl.plugin.spatialoverview.canvas(...)` takes
`workspace toggle|open|close` to change only the focused workspace's Canvas
mode, and `hud toggle` to show or hide that workspace's navigation HUD. These
actions require `canvas.workspace_bound=true`; the plugin owns this state, so
bindings do not keep a separate monitor-based open table.

`hl.plugin.spatialoverview.workspace_canvas_active()` takes no arguments and
returns true only while the focused regular workspace's workspace-bound Canvas
renderer is live and not closing. It returns false for special workspaces,
fullscreen suspension, a renderer attached to another workspace, or no live
renderer. Bindings can use it to route shared keys to Canvas actions while
Canvas renders and to native Hyprland actions otherwise. The workspace state
JSON keeps requested mode separate from live rendering: each
`workspaces[]` entry's `canvas` is the logical enabled state, while
`rendering` reports whether that workspace currently has the live renderer.
Fullscreen can therefore leave `canvas: true` and `rendering: false`; rendering
resumes when fullscreen ends.

`search [text]`, `tune`, `fill`, `pin`, `go <place>|next|prev|back`,
`send <place> [stay]`, `switch next|prev`, `fit`, `summon`, `zoom in|out`,
`pan <dir>`, `nudge <dir>`, `undo`, `redo`, `arrange`, `frame`, `land`, `back`,
`noop` (for keys that do nothing on the canvas) and `refresh`.
`hyprctl spatialoverview` prints the canvases' state as JSON. Its `workspaces`
array reports each real workspace's `id`, `name`, `active`, logical `canvas`
mode, live `rendering` state, `monitor`, `navigating`, and independent
two-number `view` state.

How each key behaves on the canvas, and what the tests check, is in
[docs/window-rules.md](docs/window-rules.md).

## Troubleshooting

- **"this build is for a different Hyprland version"**: Hyprland was updated.
  Run `scripts/install.sh` again.
- **Nothing happens on `SUPER + CTRL + G`**: check `hyprctl plugin list` and
  `hyprctl configerrors`. If you load the plugin in your own way, make sure
  `spatialoverview.lua` is loaded after it.
- **Something broke**: `scripts/uninstall.sh` puts your desktop back as it was.
  Please open an issue with your Hyprland version (`hyprctl version`).

This hooks deep into Hyprland internals, so a Hyprland update can break it
until it is updated too. It has been used daily on one setup (Omarchy,
Hyprland 0.56.2, NVIDIA, two monitors); other setups are less tested.

## Development

`make` builds `spatialoverview.so` in the checkout; `scripts/install-live.sh`
builds it and swaps it into the running session, keeping windows where they
are. The tests run a nested Hyprland in a window (`tests/*-nested.py`);
`make test-tools` builds the virtual mouse some of them use.

## Credits

Phantomat began as a fork of
[hyprland-scroll-overview](https://github.com/yayuuu/hyprland-scroll-overview)
by yayuuu (Daniel Skorupa) and its contributors, which grew out of the plugin
work of Vaxry and the Hypr Development team. Their code and history are
kept in this repository. Neither project endorses this one.

## License

BSD 3-Clause; see [LICENSE](LICENSE).
