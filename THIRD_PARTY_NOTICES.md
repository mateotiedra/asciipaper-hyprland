# Third-party notices

This add-on's original code and documentation are MIT licensed, copyright 2026 mateotiedra. Dependencies and upstream resources retain their original copyrights and licenses. The project does not claim ownership of the asciipaper engine, protocol definitions, fonts or system libraries.

## asciipaper engine

- Source: [cYoren/asciipaper-engine](https://github.com/cYoren/asciipaper-engine).
- Pinned revision: `0338b7d0a026c42027b9880537ab8c91b9c63adc` (also recorded in [UPSTREAM.json](UPSTREAM.json)).
- License: MIT, copyright (c) 2026 cYoren.
- License text: [upstream LICENSE](https://github.com/cYoren/asciipaper-engine/blob/0338b7d0a026c42027b9880537ab8c91b9c63adc/LICENSE).

The package builds the pinned engine without modifying its source and distributes its runtime resources. The payload preserves the engine's `LICENSE` and `licenses/` directory. The MIT license text in this repository's [LICENSE](LICENSE) is the same permission and warranty language, with this add-on's own copyright holder; upstream's notice must also remain with redistributed upstream code.

The engine includes a port of the [asciify](https://asciify.org) fluid scene. Its separate MIT notice is copyright (c) 2026 ayangabryl and is preserved in [`licenses/asciify-engine-MIT.txt`](https://github.com/cYoren/asciipaper-engine/blob/0338b7d0a026c42027b9880537ab8c91b9c63adc/licenses/asciify-engine-MIT.txt). The bundled runtime includes resources beyond the shark scene, so this notice remains relevant even though the wallpaper does not select the fluid scene.

## Wayland protocols

- Source: [wayland-mirror/wayland-protocols](https://github.com/wayland-mirror/wayland-protocols).
- Pinned revision: `ee78491a237eaff9389a0ccf8680521d074407d3`, version 1.49.
- License: MIT (Expat/X.org form); see the pinned [COPYING](https://github.com/wayland-mirror/wayland-protocols/blob/ee78491a237eaff9389a0ccf8680521d074407d3/COPYING), preserved in the install payload.

The project generates client bindings from protocol XML at build time. Individual protocol XML files contain their own copyright and permission notices, which also apply to the generated bindings. The aggregate `COPYING` notice names Kristian Høgsberg, Intel Corporation, Rafael Antognolli, Jasper St. Pierre, Jonas Ådahl, Jason Ekstrand, Collabora, Ltd. and Red Hat Inc.; it does not replace notices in individual XML files.

The engine also vendors [`wlr-layer-shell-unstable-v1.xml`](https://github.com/cYoren/asciipaper-engine/blob/0338b7d0a026c42027b9880537ab8c91b9c63adc/native/wlr-layer-shell-unstable-v1.xml). Its notice is reproduced here:

> Copyright © 2017 Drew DeVault
>
> Permission to use, copy, modify, distribute, and sell this software and its documentation for any purpose is hereby granted without fee, provided that the above copyright notice appear in all copies and that both that copyright notice and this permission notice appear in supporting documentation, and that the name of the copyright holders not be used in advertising or publicity pertaining to distribution of the software without specific, written prior permission. The copyright holders make no representations about the suitability of this software for any purpose. It is provided "as is" without express or implied warranty.
>
> THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

## System dependencies and fonts

The package uses system-provided Wayland, EGL/OpenGL ES and Pango/Cairo libraries. Building requires their development headers and tools, plus a C compiler, Make, Git and Python 3. FFmpeg decodes the source video during the build and can encode exported raw frames. These tools and libraries have their own licenses; installing this add-on does not relicense them. FFmpeg's applicable license depends on its build configuration; consult your distribution's package notices before redistributing it. This repository does not bundle an FFmpeg executable.

The configuration requests FantasqueSansM Nerd Font Mono, falling back to DejaVu Sans Mono. Neither font is included in this repository. Obtain fonts from their distributors and retain their licenses when redistributing font files. Pillow is an optional test dependency, not a bundled runtime component.

## Generated media

The maintainer-supplied AI-generated `assets/shark-loop.mkv` and the generated previews under `docs/media/` are distributed under this repository's [MIT license](LICENSE), allowing use, modification and redistribution under its terms. AI generation does not imply public-domain status. See [assets/README.md](assets/README.md) for provenance and build details. This grant does not relicense upstream code, resources, fonts or unrelated third-party media, or claim ownership of them.

## Documentation references

The public presentation references [mpvpaper](https://github.com/GhostNaN/mpvpaper), [swww](https://github.com/LGFae/swww) and [hyprbars](https://github.com/hyprwm/hyprland-plugins/tree/main/hyprbars) for concise build/use instructions, media presentation, scope notes and configuration-table layout. No code from those projects is bundled by this add-on, and they are not its runtime dependencies.
