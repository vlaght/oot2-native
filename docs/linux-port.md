# Linux port

This fork runs the game natively on Linux. Nothing here contains game data: you still prepare the game from your own dump (`oot2 prepare`), and the prepared folder, the AppImage's game folder and any icon you use stay on your machine (`.gitignore` keeps them out).

## What changed from upstream

| Area | Change | Where |
|---|---|---|
| Write watch | Linux 6.7+ `userfaultfd` (`WP_ASYNC`, `UFFD_USER_MODE_ONLY`, no privileges) plus `PAGEMAP_SCAN` give the service Windows gets from `GetWriteWatch`. Without it the back end falls back to host imports, which a discrete GPU reads across the bus (about 23 times slower than VRAM). | `runtime/core/plat.c` |
| Textures | ASTC is decoded on the GPU and re-encoded as BC7 mode 6: a quarter of the memory of RGBA8. Quality loss is the ASTC to BC7 step only. | `runtime/gpu/shaders/bc7_encode.comp`, `vk_texture.c` |
| Present | Bicubic scaler with sharpening where the window is larger than the picture. | `vk_scale.c`, `shaders/scale.*`, `vk_present.c` |
| Shaders | Variants missing from the packs are translated on first use by `oot2-xlate` (`OOT2_XLATE`). | `shader_translate.c`, `vk_pipeline.c` |
| Performance | See below. | `backing.c`, `texsync.c`, `nvn_ledger.c`, `nvn_tex.c` |
| Packaging | AppImage recipe and launcher. | `appimage/` |

## Performance work

Measured headless on the forest scene, from the same save, gameplay frames per second:

| Step | fps |
|---|---|
| Start of the effort | 21.4 |
| Ledger: precomputed slot order, digest memo (no `snprintf`/`qsort` per draw) | 26.0 |
| Texture layout and level offsets cached per thread | 27.9 |
| Parallel write-back copy | 37.7 |
| `texsync_regions` early-out when no page is newer | 43.6 |
| Direct write-back: the GPU copies mirror writes into guest pages over a cached import | 48.9 |
| `frames_in_flight=4` (set by the launcher; the engine default is unchanged) | ~50 |
| Whole-buffer `nvnBufferFlushMappedRange` no longer stamps pages under a precise write watch | 53.2 |
| Import cache invalidated per import (decommit record) instead of whole at each commit epoch; mirror copies behind raw stores run on the GPU timeline instead of blocking the guest thread | 60 (vsync cap) |

The last step came from a frame-time histogram rather than the average: the average hid a 35 ms hitch every eighth frame.

Tried and not kept: skipping occlusion queries, `frames_in_flight` above 4, a lazy page-protect write-back, replaying on another thread (the game reuses its buffers right after submit), and skipping the raw-store wait for pages never written back by the GPU (it hangs the load of the forest).

## Compatibility: not every distribution

The AppImage is a convenience, not a guarantee.

- **Kernel 6.7 or later** is needed for the write watch (`PAGEMAP_SCAN`, `UFFD_FEATURE_WP_ASYNC`). On an older kernel the back end falls back to imports; that path is much slower on a discrete GPU and is untested here.
- **`vm.unprivileged_userfaultfd` / seccomp**: the watch uses `UFFD_USER_MODE_ONLY`, which needs no privilege, but a sandbox that blocks `userfaultfd` (some container or Flatpak profiles) disables it.
- **Vulkan 1.3 driver** with the extensions the back end asks for (including `VK_EXT_external_memory_host`). Only an NVIDIA GPU with the proprietary driver was tested. AMD and Intel are untested.
- **glibc**: the AppImage binary is built on Fedora (a recent glibc) and bundles no libc, so it will not start on an older distribution such as Ubuntu 22.04. Build it from source on your distribution instead (below).
- **Display**: the launcher's 60 Hz switch uses `kscreen-doctor` (KDE Plasma). On other desktops it does nothing; set `OOT2_KEEP_HZ=1` or use your compositor's own setting. On a 120/144/180 Hz display the game's 60 Hz pictures are shown for uneven numbers of refreshes, which looks like stutter.
- **SDL3, dav1d and libopus** are fetched and built by `tools/fetch_*.py`. Which of them end up inside the AppImage and which are loaded from the system was not audited; check with `ldd` before relying on it.

## Building

1. Install a recent clang, the Vulkan SDK, and `uv`. Fetch the dependency kits with `tools/fetch_sdl3.py`, `tools/fetch_dav1d.py` and `tools/fetch_opus.py` and point `SDL3_DIR`, `DAV1D_DIR`, `OPUS_DIR` and `VULKAN_SDK` at them.
2. Build the shader translator (`xlate/`, a .NET publish for `linux-x64`) and set `OOT2_XLATE` to its `oot2-xlate`.
3. `uv run python tools/oot2.py prepare --dest OUT --skip-pipelines` with your dump.
4. Optional: `appimage/build.sh` (needs `appimagetool` and an `oot2.png` icon of your choice next to it; none is included). Put or link the prepared folder at `~/.local/share/oot2`, or set `OOT2_GAME`.

## Launcher settings

`OOT2_GAME` (game folder), `OOT2_KEEP_HZ=1` (leave the refresh rate alone), `OOT2_WINDOWED=1`. Development aids: `OOT2_VK_MIRROR=off` (imports only), `OOT2_VK_MIRROR_CAP_MB`, `OOT2_PACE_TRACE=<file>` (present and sync timestamps), `OOT2_REPLAY_COUNTERS=<n>`, `OOT2_REPLAY_DUMP_DIR`.

## Known issues

- `test_runtime_vk_raw` case `wbrefused` fails; it also fails on upstream's tree before these changes.
- Two compute dispatches read unmapped pointers on the raw path (the read gives zero). They were there before these changes and are unverified against a reference.
- Raw-path and mirror work in `backing.c` is intricate; the coherence tests use a stand-in write watch, so the Linux precise-watch branch is exercised only by real runs.
