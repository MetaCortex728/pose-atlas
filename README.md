# pose-atlas

A tool that turns a rigged, animated 3D character into engine-ready sprite atlases: every action in eight independent views under one versioned camera profile, with checks and a review page. It is being built as a standalone C++17 program with its own renderer.

## Origin

Han built the first version in October 2026 for his own Unreal game, with OpenAI Codex as a coding assistant and without version control: Blender and Python scripts that render a character from a fixed quarter view, an Aseprite bridge that imports the frames as an editable file with tags and a pivot slice, and validators for the resulting atlas and its metadata. Those scripts are dated 19 to 21 October 2026.

`prototype/` keeps the parts that do not depend on Blender, unchanged except for one default tool path: the projection profile and its loader (`quarter-view-profile.json`, `dan_projection.py`), the atlas and metadata validator and its tests (`check-pose-pipeline.py`, `test-pose-validator.py`), the Aseprite import bridge (`import-pose-proxy.lua`) and the pipeline runner (`run-pose-pipeline.ps1`). The Blender render script is not included: it uses Blender's Python API, which would bring Blender's GPL into this repository, and the new renderer replaces it. The prototype's tests and runner refer to game assets that are not part of this repository, so they document the original pipeline rather than run here.

## Quick start

Build (below), then turn the generated test character into a validated atlas:

```sh
build/default/apps/pose-atlas make-test-rig out/test-rig.glb
build/default/apps/pose-atlas render out/test-rig.glb --profile profiles/quarter-view-gltf.json --actions idle,run --out out/rig
build/default/apps/pose-atlas validate out/rig
```

`render` samples `idle` and `run` (8 poses each by default), renders them in the eight views of the profile and writes `out/rig/atlas.png` (rows = views, columns = poses), `atlas.json` (Aseprite-style frames, tags and `foot` pivot slice), `atlas.aseprite` (one frame per pose and view, one tag per state, a `foot` slice and an empty "Manual corrections" layer) and `pose-contract.json` (profile, clocks, sources and hashes). It validates the result before it returns. Renders are cached in `out/rig/cache` by content hash, so running it again reports `0 rendered, 16 reused`, and changing one action (for example `--poses idle=8,run=6`) re-renders only that action. `validate` re-checks a directory at any time. Run `pose-atlas help` for all options.

Palette-locked pixel art: give a palette (JSON `{"colors": ["#rrggbb", ...]}` or a PNG whose distinct opaque pixels are the colours, such as a palette exported from Aseprite) and every opaque pixel of the atlas uses one of its colours, with alpha either 0 or 255:

```sh
build/default/apps/pose-atlas render out/test-rig.glb --profile profiles/quarter-view-gltf.json --actions idle,run \
    --palette palettes/test-rig.json --palette-stage before_after --outline --out out/rig-palette
```

`palettes/test-rig.json` holds the 16 colours of the generated rig, including the four of the checker texture on its torso. `--palette-stage` chooses where colours are snapped to the nearest palette colour (in Oklab): `before` the downsample (on the supersampled render), `after` it (on the cell) or `before_after` (both, the default); `--alpha-threshold` (default 0.5) sets the coverage at which an edge pixel becomes opaque. The run prints a line such as `palette: before_after, 16 colours: … opaque pixels, … off-palette before the final snap, … with partial alpha before thresholding, 0 off-palette in the output`, the contract gets the numbers per action and view under `palette_report`, and `validate` checks the palette lock. For the rig, snapping only `before` leaves about 29 % of the opaque pixels off the palette (blends of neighbouring regions by the Mitchell filter) and so fails validation; `before_after` leaves none (see [docs/rendering.md](docs/rendering.md#palette-locked-output)).

## Build and test

You need CMake 3.21 or newer, Ninja, GCC or Clang with C++17, and OpenGL through EGL with Mesa for the headless renderer (on Ubuntu: `libegl-dev libopengl-dev libegl-mesa0 libgl1-mesa-dri`). The renderer always uses Mesa's software device (llvmpipe), so no GPU or display is needed and output does not depend on the machine's GPU. All other dependencies are vendored under `third_party/` (see [NOTICE](NOTICE)).

```sh
cmake --preset default && cmake --build --preset default && ctest --preset default
```

Presets: `default` (GCC), `clang`, `asan` (GCC with AddressSanitizer and UndefinedBehaviorSanitizer), and `ci-default`, `ci-clang`, `ci-asan`, the same with warnings as errors, which CI runs on Linux. The build directory is `build/<preset>`; the tool is `build/<preset>/apps/pose-atlas`. The `Dockerfile` provides the Ubuntu 22.04 toolchain that CI uses.

Documentation: [docs/profile.md](docs/profile.md) describes the camera profile; [docs/clocks.md](docs/clocks.md) the glTF loader and the three clocks of a sampling plan (source time, pose index, runtime cadence); [docs/rendering.md](docs/rendering.md) the renderer, base colour textures and palette-locked output; [docs/atlas.md](docs/atlas.md) the atlas, its metadata and contract, and the validation rules.

The tests use a procedurally generated character instead of any game asset: `build/<preset>/apps/pose-atlas make-test-rig out/test-rig.glb` writes it (an asymmetric box figure on an eight-joint skin, faces +Z, with a pivot marker at the feet and the animations `idle`, 2 s, and `run`, 0.8 s). Its torso carries a generated 2×2 checker as a base colour texture. It is generated by `src/test_rig.cpp` and carries the repository's licence, as does `palettes/test-rig.json`.

## Licence

Licensed under the Apache License, Version 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
