# Loading and clocks

`pose_atlas::load_gltf` reads `.gltf` and `.glb` files with cgltf: node hierarchy and rest transforms, triangle meshes with materials' base colours, skins (joints and inverse bind matrices) and animations (translation, rotation and scale channels with `LINEAR`, `STEP` or `CUBICSPLINE` interpolation; morph-target weight channels are counted and ignored). `evaluate` gives every node's transform at any source time, either clamped to the clip or wrapped as a loop; `pose_primitive` returns linear-blend skinned positions and normals in model space.

## Three clocks

A sampling plan (`plan_sampling`, or `pose-atlas plan`) keeps three clocks apart and reports them side by side for every sampled pose:

| Clock | Unit | Meaning |
|-------|------|---------|
| Source | seconds | Time on the animation's own clock in the glTF file. |
| Pose | index 0..N-1 | Which of the N sampled poses, i.e. the sprite frame within the action. |
| Runtime | ms or m | How the game advances through the poses. |

The runtime cadence is one of:

- `source_rate` (`--rate R`, default 1): runtime time is source time divided by R;
- `fixed_frame` (`--frame-ms MS`): every pose is shown for the same time, whatever its source step;
- `distance` (`--cycle-m M`): one pass over the poses covers M metres, so each pose covers M / intervals metres. With `--speed-mps V` the plan also gives durations at that reference speed and the implied playback rate of the source clip.

The cadence never changes which poses are sampled, and the pose count never changes the source clock.

## End point

With the end point **excluded** (the default, for loops), N poses sit at `start + i * D / N`; the end of the clip, which equals the start for a closed loop, is not repeated. With it **included** (`--include-end`, for one-shot actions), N ≥ 2 poses sit at `start + i * D / (N - 1)` and the last is exactly the end. The plan measures loop closure (how far the end pose is from the start pose over all nodes) and warns when an included end point repeats the first pose or when an excluded one hides a jump.

```sh
pose-atlas make-test-rig out/test-rig.glb
pose-atlas plan out/test-rig.glb --action run --poses 8 --cycle-m 1.6 --speed-mps 4
```
