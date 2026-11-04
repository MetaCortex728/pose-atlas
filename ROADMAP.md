# Roadmap

pose-atlas turns a rigged, animated 3D character (glTF/GLB) into engine-ready sprite atlases under one versioned camera profile. It does not replace painting (Aseprite), modelling and rigging (Blender), or a game engine's gameplay code.

## Stage 1: core pipeline (done)

1. **Profile and contract:** one versioned profile (orthographic camera, elevation, azimuth, eight independent screen headings, cell size, ground pivot, metres per pixel, supersampling, filter, zoom set) with validation; the atlas contract it produces.
2. **Loader and clocks:** glTF/GLB meshes, skins and animations; explicit source time, sample time and runtime cadence, reported side by side.
3. **Headless renderer:** deterministic OpenGL rendering of each action in eight views, alpha-safe (premultiplied) downsampling from a supersampled image, flat region palettes as the first style.
4. **Export:** atlas PNG with Aseprite-style JSON (frames, tags, pivot slice, durations), and `.aseprite` files with one tag per state, a pivot slice and an empty correction layer.
5. **Checks and cache:** cell cropping, pivot drift, state coverage, loop closure; re-render only actions and views whose inputs changed (content hashes).

Done when a procedurally generated test rig with two actions becomes a validated atlas and `.aseprite` file in CI.

Also done, ahead of Stage 2: base colour textures and palette-locked output with a user-supplied palette (the first part of item 6), and stable frame keys with hand corrections carried across re-renders, marked stale when the render under them changed.

## Stage 2: quality and review (next)

6. **Colour from the approved illustration:** register the character's approved front and back illustrations to the model's rest pose with landmarks, extract per-region palettes and tone ramps from them, render with those palettes (model textures only separate regions), add outline controls, and report per-region colour differences between each rendered view and the illustration. Snapping within each region's ramp replaces today's nearest colour of the whole palette, which can pick a colour from another region (on a real character, soles snapped to a hair colour). The illustrations are user input and stay out of the repository; tests use generated ones (about 8 to 10 hours).
7. **Motion diagnostics:** velocity continuity across loops and transitions, foot contact and sliding (about 6 hours).
8. **Review page:** a self-contained web page showing states side by side at native size over user-supplied backgrounds at several zoom levels (about 5 hours). Two kinds of zoom: the profile's `zoom_set` (integer pixel scales such as 1×, 2×, 3×) and the game camera's zoom levels (for example 90, 100 and 110 %), which need a profile field of their own.
9. **Harmony measurements:** apparent pixel density, palette distance and edge treatment against scene images (about 4 hours).
10. **Engine contract validator:** checks an existing atlas and metadata against the profile without rendering (about 3 hours).
11. **Effect baking:** particle-based effects rendered to sprite sequences with exact loops (about 5 hours).
