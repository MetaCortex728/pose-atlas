# pose-atlas

A tool that turns a rigged, animated 3D character into engine-ready sprite atlases: every action in eight independent views under one versioned camera profile, with checks and a review page. It is being built as a standalone C++17 program with its own renderer.

## Origin

Han built the first version in October 2026 for his own Unreal game, with OpenAI Codex as a coding assistant and without version control: Blender and Python scripts that render a character from a fixed quarter view, an Aseprite bridge that imports the frames as an editable file with tags and a pivot slice, and validators for the resulting atlas and its metadata. Those scripts are dated 19 to 21 October 2026.

`prototype/` keeps the parts that do not depend on Blender, unchanged except for one default tool path: the projection profile and its loader (`quarter-view-profile.json`, `dan_projection.py`), the atlas and metadata validator and its tests (`check-pose-pipeline.py`, `test-pose-validator.py`), the Aseprite import bridge (`import-pose-proxy.lua`) and the pipeline runner (`run-pose-pipeline.ps1`). The Blender render script is not included: it uses Blender's Python API, which would bring Blender's GPL into this repository, and the new renderer replaces it. The prototype's tests and runner refer to game assets that are not part of this repository, so they document the original pipeline rather than run here.
