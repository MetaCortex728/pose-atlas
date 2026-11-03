# Atlas, metadata and contract

`render_atlas` samples each action with its sampling plan (see [clocks.md](clocks.md)), renders every pose in the eight views of the profile and lays the cells out like the prototype: **rows are views** in heading order (`e, ne, n, nw, w, sw, s, se`), **columns are poses**, the poses of all actions one after another (`idle0 … idle7, run0 … run7`). Frame *i* of the atlas is row *i* div columns, column *i* mod columns; its name is `<pose>.<view>`, e.g. `run3.nw`.

`export_atlas` writes into the output directory:

| File | Content |
|------|---------|
| `atlas.png` | All cells, RGBA, straight alpha, sRGB. |
| `atlas.json` | Aseprite-style JSON (`json-array`): `frames` with `filename`, `frame`, `rotated: false`, `trimmed: false`, `spriteSourceSize`, `sourceSize`, `duration` (ms); `meta` with `image`, `size`, `frameTags` (one per pose and view, `from` = `to` = its frame) and `slices` (one `foot` slice keyed on frame 0, bounds = the cell, pivot = the profile's ground pivot). |
| `atlas.aseprite` | The same frames as an editable Aseprite file, written directly from the [documented format](https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md) (RGBA, sRGB colour profile): one frame per cell in atlas order with its duration; a base layer `Render` holding the render and an empty layer `Manual corrections` above it; one tag per state, `<action>.<view>` over that action's poses in that view; a `foot` slice over the cell with the pivot. Tested by reading it back (with the library's reader and with an independent parser in the tests); not yet opened in Aseprite itself. |
| `pose-contract.json` | `format: "pose-atlas-contract"`, `version: 1`; tool and renderer; `sources` (model path and SHA-256, the profile's values and hash, the render settings and hash; with a palette, `palette` with its path, `sha256` and `colors`); cell, pivot, metres per pixel, cell span; views, columns, pose names; per action the three clocks, loop closure, plan warnings and the render key of each view; per frame its action, pose index, view, source time, runtime start, duration and `projected_heading`; `files` with the SHA-256 of every other output; with a palette, `palette_report`: `stage`, `alpha_threshold`, `palette_sha256`, `outline_colour` (or null), per (action, view) `renders` and the `total` of `opaque`, `off_palette_before_snap`, `partial_alpha` and `off_palette_output` (see [rendering.md](rendering.md#palette-locked-output)). |

Durations are whole milliseconds: the runtime start and end of each pose are rounded and subtracted, so the durations of an action add up to its rounded cycle. A distance cadence without a reference speed has no durations and is refused.

## Validation

`pose-atlas validate <dir>` (`validate_atlas`) ports the rules of the prototype's `check-pose-pipeline.py` that apply here and reports every problem it finds:

- contract format and version; the SHA-256 of every file listed in the contract;
- the contract's cell, pivot, metres per pixel and cell span agree with its embedded profile ("physical framing");
- eight views in heading order; pose names are the actions' poses in column order; every action has a render in every view (state coverage);
- frame count = columns × views in both JSON files; `meta.size` and the PNG size are the atlas size;
- one tag per pose and view on its own frame only; exactly one `foot` slice with one key on frame 0, the pivot and full-cell bounds;
- per frame: name, rectangle, untrimmed, unrotated, full source size, zero source offset, duration equal to the contract's and at least 1 ms; `projected_heading` is the unit vector of its screen heading (within 1e-5) and follows from the profile;
- per cell: not blank, and no alpha in the outermost rows and columns (no clipping);
- `atlas.aseprite`: cell-sized sprite, one frame per cell with the contract's duration, the two layers with nothing on the corrections layer, one tag per state over its frames, the `foot` slice with the pivot, and every base-layer cel holding exactly the atlas cell's RGBA (the prototype's "Import/export changed raw RGBA pixels" rule, which compared its raw frame files with the cells exported back from Aseprite);
- the eight views of every pose differ (directions changed), and no action of several poses shows a single image throughout a view (poses changed).
- when the contract names a palette (`sources.palette`): its `sha256` is the hash of its colours and matches the render settings' `palette_sha256`; every opaque atlas pixel is a palette colour (or the outline colour, when the render settings have the outline on) and every alpha is 0 or 255 (palette lock). The first five offenders are reported with atlas coordinates, frame and cell coordinates, followed by the totals.

Not ported: the prototype's fixed pose-name and schema lists, its 64/96 px cells and 208 cm span, and its binary-alpha rule for renders without a palette (filtered edges have partial alpha); with a palette, hard alpha is required. Its "every cell differs" rule became the two rules above, because a real animation can repeat a pose; the rig's run does, from the side.

## Cache

With a cache directory (`pose-atlas render` uses `<out>/cache` unless given `--cache`), each (action, view) is keyed by SHA-256 over the model file's hash, the profile's hash (all its values, including the filter and supersampling), the render settings' hash (which includes the texture filter, the palette stage, the alpha threshold and the palette's content hash), the renderer description (GL renderer and Mesa version), the action name and its sampled source times, and the view. A stored strip `<key>.png` with that action's poses in that view (and, with a palette, its statistics in `<key>.palette.json`) is reused; otherwise the poses are rendered and the strip is stored. So changing one action's sampling re-renders only that action's eight views; changing the runtime cadence re-renders nothing (it only changes durations); changing the model file, the profile, the settings or the Mesa version re-renders everything. Entries that are missing, unreadable or of the wrong size are rendered again. The contract records each render's key, but not whether it was reused, so a cached and an uncached export are byte-identical.

The model file is hashed as a whole, so editing one animation in the GLB also re-renders the other actions; keying by the data each action actually uses is possible later.
