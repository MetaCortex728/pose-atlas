# Camera profile

A profile fixes everything that must stay the same across every action and view of a character: the camera, the cell and its physical scale, and the resampling and zoom settings. It is a JSON file; `profiles/quarter-view.json` carries the camera values of the October 2026 prototype (`prototype/quarter-view-profile.json`), and `profiles/quarter-view-gltf.json` is the same camera for models in glTF axes.

Check a profile with `pose-atlas profile check <file>`; print the eight camera guides with `pose-atlas profile guides <file>`.

| Field | Meaning |
|-------|---------|
| `format`, `version` | `"pose-atlas-profile"` and `1`. Other versions are refused rather than guessed at. |
| `id`, `description` | Name recorded in outputs; free text (optional). |
| `projection` | `"orthographic"` (the only projection). |
| `elevation_degrees` | Camera elevation above the ground plane, strictly between 0 and 90. |
| `base_azimuth_degrees` | Camera azimuth about world up before the per-heading rotation. |
| `camera_distance_m` | Distance from the target to the camera (orthographic, so it only affects clipping). |
| `screen_headings` | Exactly `["e", "ne", "n", "nw", "w", "sw", "s", "se"]`; heading *i* is *i* × 45° counter-clockwise from screen right. |
| `character_forward`, `world_up` | Model axes, each one of `+X -X +Y -Y +Z -Z`, perpendicular to each other. Blender exports use `-Y`/`+Z`; glTF uses `+Z`/`+Y`. |
| `cell_pixels` | Side of the square sprite cell, 8 to 4096. |
| `ground_pivot_pixels` | `[x, y]` pixel of the model origin in the cell, Y down, strictly inside the cell. |
| `meters_per_pixel` | Physical size of one output pixel. The cell spans `cell_pixels × meters_per_pixel` metres. |
| `supersampling` | Render at this many times the cell resolution before downsampling, 1 to 16. |
| `resampling_filter` | Downsampling filter: `box`, `triangle`, `mitchell` (Mitchell–Netravali, B = C = 1/3; the default) or `lanczos3`. Applied to premultiplied alpha. |
| `zoom_set` | Strictly increasing integer display zooms (1 to 16) for review output. |

Unknown fields are errors, so typos do not pass silently. All problems are reported at once, each as `<file>: <field>: <problem>`.

## Camera guides

For each screen heading the model stays fixed and the camera orbits world up so that the character's forward axis appears on screen along that heading. Because the camera looks down at the ground, a direction on the ground is foreshortened on screen; the guide inverts this foreshortening when it picks the camera rotation, so `ne` really points up and to the right at 45° in the image, not at the angle of the ground direction. This is a port of `camera_guide` and `project` in `prototype/dan_projection.py`. The tests compare it with values from the prototype's Python (`tests/fixtures/make_camera_fixture.py`, which CTest reruns with `--check` when Python is available).
