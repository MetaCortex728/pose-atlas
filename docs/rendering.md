# Rendering

`pose_atlas::Renderer` draws a posed model for one camera guide of the profile and returns the cell as 8-bit RGBA.

- **Context.** OpenGL 3.3 core through EGL on Mesa's software device (`EGL_MESA_device_software`, llvmpipe), chosen explicitly even when a GPU is present, so that the rasteriser is the same everywhere. No window system or display is needed. `Renderer::description()` gives `GL_RENDERER; GL_VERSION`; outputs record it.
- **Geometry.** Skinning runs on the CPU (`pose_primitive`, linear blend). Each camera is the profile's orthographic guide: the cell spans `cell_pixels × meters_per_pixel` metres, the model origin lands on `ground_pivot_pixels`, depth runs from the camera to twice `camera_distance_m` behind it. Pixel *i* covers [*i*, *i* + 1), so the pivot (128, 208) is a pixel corner.
- **Supersampling.** The scene is rendered at `cell_pixels × supersampling` into a 32-bit float target in linear light, with alpha 1 where covered (premultiplied trivially).
- **Downsampling.** On the CPU, separably, in double precision, with the profile's `resampling_filter` (`mitchell` = Mitchell–Netravali B = C = 1/3 by default; also `box`, `triangle`, `lanczos3`), on premultiplied colour so that transparent pixels do not darken edges. Outside the image counts as transparent. Results are clamped (alpha to [0, 1], colour to [0, alpha]), un-premultiplied, encoded as sRGB and rounded; pixels whose alpha rounds to 0 are (0, 0, 0, 0).
- **Style `flat-v2`.** Each region is its material's base colour without lighting: `baseColorFactor` times `pbrMetallicRoughness.baseColorTexture` when there is one. With the outline on, the cell becomes hard-edged: pixels with alpha ≥ 128 become opaque, a one-pixel 4-connected ring around them takes the outline colour, and everything else becomes transparent. (`flat-v1` had no textures; the style name is part of every cache key.)
- **Base colour textures.** PNG and JPEG images (stb_image) from a buffer view, a `data:` URI or a file next to a `.gltf` file are uploaded as `SRGB8_ALPHA8`, so the sampler decodes sRGB to linear light before filtering; the factor multiplies in linear light. The texture's `TEXCOORD_n` set is used, with `KHR_texture_transform` (offset, rotation, scale and its `texCoord` override) applied to the coordinates when the model is loaded. Sampler wrap modes (repeat, clamp to edge, mirrored repeat) are honoured; the sampler's filters are not: the render setting `texture_filter` chooses `nearest` (default, hard texel edges) or `bilinear` sampling at the supersampled resolution, without mipmaps, and it is part of the settings hash and so of the cache key. Alpha mode `MASK` discards fragments below `alphaCutoff`; `OPAQUE` and `BLEND` are drawn opaque. KTX2 (`KHR_texture_basisu`) and WebP images are refused with an error rather than rendered white.
- **Determinism.** The same model, pose, profile and settings give byte-identical PNGs (tested in the same process and with a fresh context). Different Mesa or LLVM versions may rasterise edges differently; that is why outputs record the renderer description and the render cache keys on it.

## Palette-locked output

With a palette (`--palette`, 2 to 256 distinct sRGB colours from JSON `{"colors": ["#rrggbb", ...]}` or from the distinct opaque pixels of a PNG), the render setting `palette_stage` decides where colours are snapped to the nearest palette colour, measured as Euclidean distance in Oklab (ties go to the earlier colour):

| `palette_stage` | Pipeline |
|---|---|
| `none` | no palette (the behaviour without `--palette`) |
| `before` | snap every covered supersampled pixel (its un-premultiplied colour), then downsample |
| `after` | downsample, then snap every cell pixel |
| `before_after` | both (the default when a palette is given) |

Whenever a palette is active, the cell's alpha is then thresholded at `alpha_threshold` coverage (default 0.5: alpha ≥ 128 of 255) to exactly 0 or 255, and fully transparent pixels become (0, 0, 0, 0). The outline, if on, comes last, so its colour is added to the allowed colours. The palette enters the settings as its content hash (`palette_sha256`: SHA-256 of the canonical JSON of the colour list, the same for a PNG and a JSON file with the same colours in the same order), so it is part of every cache key; output stays deterministic (tested in the same process and with a fresh context).

Each palette render is measured per cell and summed per (action, view): `opaque` (opaque output pixels), `off_palette_before_snap` (pixels that end up opaque whose colour is not in the palette, after the downsample and before the final snap; for `before`, which has no final snap, in the output), `partial_alpha` (pixels with alpha strictly between 0 and 255 before thresholding) and `off_palette_output` (opaque output pixels outside the palette and outline colour). The render prints the totals in one line and writes all numbers to the contract's `palette_report`.

**Is snapping before the downsample enough? No.** The generated rig with its own 16 colours (`palettes/test-rig.json`), actions `idle` and `run` with 8 poses each, eight views, 256 px cells at 4× supersampling with the Mitchell filter:

| stage | opaque | off-palette before the final snap | partial alpha before thresholding | off-palette in the output |
|---|---:|---:|---:|---:|
| `before` | 709 429 | 206 476 (29.1 %) | 125 710 | 206 476 |
| `before_after` | 709 429 | 206 476 | 125 710 | 0 |
| `after` | 709 429 | 211 400 | 125 710 | 0 |

Snapping the supersampled render makes every region one palette colour, but the downsample blends neighbouring regions wherever the filter's footprint (2 output pixels each way for Mitchell) holds more than one colour or coverage, and those blends are off the palette. The tests check this exactly: every off-palette pixel left by `before` has a non-uniform footprint in the snapped supersampled render, and every pixel with a uniform footprint is on the palette. Only a final snap removes them, so `before_after` (or `after`) is needed for palette-locked output, and `pose-atlas validate` fails a `before` export. Partial alpha is the same for every stage (snapping does not change coverage); the alpha threshold removes it.
