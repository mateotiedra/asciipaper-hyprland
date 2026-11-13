# Configuration

The installed spec is `${XDG_CONFIG_HOME:-$HOME/.config}/asciipaper-hyprland/wallpaper.json`. The [source template](../config/wallpaper.json) uses relative shader and media paths; installation resolves package assets to the installed data directory. Keep your custom media outside package-owned asset paths.

The engine watches the spec directory and reloads on a completed write or atomic file replacement. Save valid JSON: no comments or trailing commas. Uniform-only edits, including all five rain controls, apply without restarting the service.

## Matrix rain

These keys belong in the existing `uniforms` object. Preserve the other uniforms when editing:

```json
{
  "matrixBrightness": 0.7,
  "matrixDensity": 2.3,
  "matrixSpeed": 2.3,
  "matrixTrailLength": 5,
  "matrixChangeInterval": 2.4
}
```

| Uniform | Type | Shipped value | Behavior |
| --- | --- | --- | --- |
| `matrixBrightness` | number | `0.7` | Blends rain ink toward the dark background; clamped to 0…1. Set 0 to hide rain. |
| `matrixDensity` | number | `2.3` | Multiplies stream count relative to the shader's original sparse layout. Nonpositive values disable rain; spacing never falls below four columns. |
| `matrixSpeed` | number | `2.3` | Typical descent in rows per video-clock second, with small per-stream variation. Nonpositive values stop descent. |
| `matrixTrailLength` | number | `5` | Typical length in character cells, rounded and varied by one cell per stream, with a one-cell minimum. |
| `matrixChangeInterval` | number | `2.4` | Typical video-clock seconds between glyph mutations, staggered per cell. Nonpositive values disable time-driven mutations. |

The rain clock is scene time multiplied by `uniforms.speed` (shipped as `0.7`). With that speed, the typical `2.3` rows per video-clock second is about `1.61` rows per scene second. Changing `speed` changes both video playback and rain timing. Stopping descent does not stop glyph mutations; set `matrixChangeInterval` to 0 too if you want both to stop.

Rain appears only in empty video cells and does not replace visible shark cells. The cursor fish overlays both. Rain uses the existing character ramp and the `tint`/`tint2` colors rather than a separate font or palette.

JSON uniforms override the composed shader's first-line defaults. The source rain shader's fallback values (`0.5`, `1.35`, `2`, `5`, `2.4`) differ from the deliberately brighter, denser shipped spec. Use the installed JSON to tune the look.

## Appearance defaults

These are the values in the shipped spec, not guarantees about upstream defaults.

| Top-level key | Shipped value | Purpose |
| --- | --- | --- |
| `shader` | `wallpaper.glsl` | Composed media, rain and fish shader; resolved during install. |
| `media` | `media/shark-loop.mkv` | Included generated video; resolved during install. |
| `charset` | `" .,:;i+=xX#@"` | Character ramp, including the initial space. The fish and rain shaders assume this ramp. |
| `font` | `FantasqueSansM Nerd Font Mono, DejaVu Sans Mono` | Preferred monospace font and fallback. |
| `cell` | `12` | Character-cell size. |
| `aspect` | `0.55` | Character-cell aspect ratio. |
| `fill` | `0` | Cell fill setting. |
| `background` | `#101014` | Background color; the rain shader's fade destination also uses this color. |
| `effects` | `{}` | No additional postprocessing effects. |
| `interaction` | `none` | Disables upstream interaction effects; the add-on handles the fish separately. |

| Other uniform | Shipped value |
| --- | --- |
| `lens` | `0` |
| `contrast` | `4.0` |
| `brightness` | `1.41` |
| `gamma` | `1` |
| `threshold` | `0.16` |
| `fit` | `0` |
| `colorMode` | `2` |
| `tint` | `#c47a9b` |
| `tint2` | `#b8a0d8` |
| `backdrop` | `0` |
| `speed` | `0.7` |

For the underlying media/look controls, consult the pinned engine's [look catalog](https://github.com/cYoren/asciipaper-engine/blob/0338b7d0a026c42027b9880537ab8c91b9c63adc/wallpapers/lib/looks.json) and [spec documentation](https://github.com/cYoren/asciipaper-engine/tree/0338b7d0a026c42027b9880537ab8c91b9c63adc). Changing the ramp or background also requires understanding the shader assumptions above. Fish geometry, tail motion and fixed-right lighting are implemented in the C motion model and fish shader, not exposed as JSON controls.

## Alternate configuration

Stop the managed service before a foreground desktop run:

```sh
~/.local/bin/asciipaper-hyprland stop
~/.local/bin/asciipaper-hyprland run \
  --config "${XDG_CONFIG_HOME:-$HOME/.config}/asciipaper-hyprland/wallpaper.json"
```

`run` requires the Hyprland environment and exactly one active, unrotated monitor. It does not stop competing wallpapers. The `start`, `stop` and `status` commands manage only this package's user service; `--config` belongs to `run` and `render`.

## Deterministic exports

```sh
~/.local/bin/asciipaper-hyprland render preview.png \
  --size 1280x720 --seconds 4 --pointer 0.7,0.45
```

| Option | Meaning |
| --- | --- |
| `OUTPUT` | Filename for a PNG snapshot. Use `-` only with `--record` for raw video on stdout. |
| `--config PATH` | Use a different wallpaper spec. |
| `--size WxH` | Output dimensions, at least `16x16`; defaults to `1920x1080`. |
| `--seconds N` | Simulated scene time (or recording duration); defaults to `3`. |
| `--pointer X,Y` | Simulated normalized pointer coordinates. |
| `--record FPS` | Export raw RGBA video at 1…120 frames per second; requires `OUTPUT` to be `-`. |
| `--warmup N` | Warmup time for recording. |

For example, encode a raw recording with FFmpeg, explicitly matching frame dimensions and frame rate:

```sh
~/.local/bin/asciipaper-hyprland render - \
  --size 1280x720 --seconds 6 --pointer 0.7,0.45 --record 24 --warmup 2 \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 24 -i - \
      -an -c:v libx264 -pix_fmt yuv420p demo.mp4
```

For headless Mesa, prefix the render command with `LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless`. Rendering uses `ASCIIPAPER_FISH_MODE=native`: renderer-provided time, pointer and aspect, without starting the Hyprland tracker. Desktop runs default to `global` mode. Explicit modes other than `global` and `native` are rejected. A backwards native scene-time reset also resets fish motion; desktop mode retains its monotonic motion clock.

The launcher scopes `LD_PRELOAD` to its native renderer process. Do not export the interposer session-wide.

## Troubleshooting

- If startup rejects your monitor layout, use exactly one active, unrotated output. The package does not reconfigure monitors for you.
- If the service cannot find the session, run `start` again from a terminal inside Hyprland so it imports the current environment. Use `run` for direct error output.
- If another wallpaper appears instead, stop that wallpaper manually. This package never kills other wallpaper processes.
- If rain changes have no effect, check that the keys are numbers inside `uniforms`, the file is valid JSON, and you edited the config used by the running process.
- If the font differs from the previews, install your preferred font or use the included DejaVu Sans Mono fallback. Font metrics affect the appearance.
- If headless rendering cannot initialize EGL, check your Mesa/EGL installation and try the software-rendering environment above.

Upgrades preserve edited specs. Uninstall preserves configuration and modified files by default; see the [lifecycle instructions](../README.md#upgrade-rollback-and-uninstall).
