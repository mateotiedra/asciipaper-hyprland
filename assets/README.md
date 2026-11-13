# Media

`shark-loop.mkv` is an AI-generated shark-loop video supplied by the maintainer and distributed under this repository's [MIT license](../LICENSE). You may use, modify and redistribute it under those terms. It is included locally; the build does not download arbitrary videos. AI generation does not imply public-domain status.

The build decodes this video using the pinned engine's `tools/import_media.py` at a frame width of 384 and 24 frames per second. The resulting `shark-loop.mkv.frames` is a generated runtime asset under `build/stage/media/`, not a separate source video. The package keeps its own wallpaper spec instead of the importer's generated look JSON.

The README previews under `docs/media/` (`demo.gif`, `demo.mp4`, `overview.png` and `fish-detail.png`) are generated from the packaged native renderer with simulated pointer input. They show the wallpaper itself, not screenshots of personal applications. The maintainer distributes these generated previews under the same [MIT license](../LICENSE).

The project code is also covered by that license. The engine and its bundled resources retain their own licenses; see [third-party notices](../THIRD_PARTY_NOTICES.md). The media grant does not relicense upstream code, fonts or unrelated third-party media, or claim ownership of them.
