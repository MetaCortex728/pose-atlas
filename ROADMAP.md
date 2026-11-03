# Roadmap

pose-atlas turns a rigged, animated 3D character (glTF/GLB) into engine-ready sprite atlases under one versioned camera profile. It does not replace painting (Aseprite), modelling and rigging (Blender), or a game engine's gameplay code.

## Stage 1: core pipeline (in progress)

1. **Profile and contract:** one versioned profile (orthographic camera, elevation, azimuth, eight independent screen headings, cell size, ground pivot, metres per pixel, supersampling, filter, zoom set) with validation; the atlas contract it produces.
2. **Loader and clocks:** glTF/GLB meshes, skins and animations; explicit source time, sample time and runtime cadence, reported side by side.
3. **Headless renderer:** deterministic OpenGL rendering of each action in eight views, alpha-safe (premultiplied) downsampling from a supersampled image, flat region palettes as the first style.
4. **Export:** atlas PNG with Aseprite-style JSON (frames, tags, pivot slice, durations), and `.aseprite` files with one tag per state, a pivot slice and an empty correction layer.
5. **Checks and cache:** cell cropping, pivot drift, state coverage, loop closure; re-render only actions and views whose inputs changed (content hashes).

Done when a procedurally generated test rig with two actions becomes a validated atlas and `.aseprite` file in CI.

## Stage 2: quality and review

6. **Styles:** palette ramps and outline controls, recorded in the output (about 6 hours).
7. **Motion diagnostics:** velocity continuity across loops and transitions, foot contact and sliding (about 6 hours).
8. **Review page:** a self-contained web page showing states side by side at native size over user-supplied backgrounds at several zoom levels (about 5 hours).
9. **Harmony measurements:** apparent pixel density, palette distance and edge treatment against scene images (about 4 hours).
10. **Engine contract validator:** checks an existing atlas and metadata against the profile without rendering (about 3 hours).
11. **Effect baking:** particle-based effects rendered to sprite sequences with exact loops (about 5 hours).
