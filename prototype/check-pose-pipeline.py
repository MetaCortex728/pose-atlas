"""Validate a fixed-cell Blender/Aseprite proxy without modifying any artwork."""
import argparse
import json
import math
from pathlib import Path

from PIL import Image


def validate(root):
    contract = json.loads((root / "pose-contract.json").read_text(encoding="utf-8"))
    metadata = json.loads((root / "pose-proxy.json").read_text(encoding="utf-8"))
    frames, meta = metadata["frames"], metadata["meta"]
    views = contract["view_count"]
    poses = contract.get("pose_names", ["idle", "windup", "impact", "recover", "dash"])
    columns = len(poses)
    assert poses in (["idle", "windup", "impact", "recover", "dash"],
                     ["idle", "windup", "impact", "recover", "dash", "walk0", "walk1", "walk2", "walk3"],
                     ["idle", "windup", "impact", "recover", "dash"] + [f"walk{i}" for i in range(8)],
                     ["idle", "windup", "impact", "recover", "dash"] + [f"walk{i}" for i in range(16)],
                     ["idle", "windup", "impact", "recover", "dash"] + [f"walk{i}" for i in range(16)]
                     + ["attack_ready", "attack_extend", "recover_mid", "recover_end"],
                     ["idle", "windup", "impact", "recover", "dash"] + [f"walk{i}" for i in range(16)]
                     + ["attack_ready", "attack_extend", "recover_mid", "recover_end"]
                     + [f"{stage}{phase}" for phase in range(16) for stage in ("attack_entry", "attack_settle")])
    if columns in (13, 21, 25, 57):
        assert contract["schema"] == {13: 4, 21: 5, 25: 6, 57: 7}[columns] and views == 8
        gait = contract["locomotion"]
        assert gait["pose_count"] == (16 if columns >= 25 else columns - 5)
        assert isinstance(gait["cycle_cm"], (int, float)) and not isinstance(gait["cycle_cm"], bool)
        assert math.isfinite(gait["cycle_cm"]) and 0 < gait["cycle_cm"] <= 1000
    assert views in (1, 8)
    assert len(frames) == len(contract["frames"]) == columns * views
    size = contract["cell"][0]
    assert size in (64, 96) and contract["cell"] == [size, size]
    pivot = [size // 2, size * 13 // 16 if columns in (21, 25, 57) else size * 7 // 8]
    assert contract["pivot"] == pivot
    assert math.isclose(contract["cm_per_pixel"] * size, 208.0, abs_tol=1e-6), "Physical framing changed"
    assert len(meta["frameTags"]) == columns * views
    tags = {tag["name"]: (tag["from"], tag["to"]) for tag in meta["frameTags"]}
    assert len(meta["slices"]) == 1 and meta["slices"][0]["name"] == "foot", "Expected exactly one foot slice"
    keys = meta["slices"][0]["keys"]
    assert len(keys) == 1 and keys[0]["frame"] == 0 and keys[0]["pivot"] == {"x": pivot[0], "y": pivot[1]}
    assert keys[0]["bounds"] == {"x": 0, "y": 0, "w": size, "h": size}
    with Image.open(root / "pose-proxy.png") as source:
        atlas = source.convert("RGBA")
    assert atlas.size == (size * columns, size * views)
    stats, decoded = [], []
    for index, (frame, item) in enumerate(zip(frames, contract["frames"])):
        x, y = (index % columns) * size, (index // columns) * size
        assert frame["frame"] == {"x": x, "y": y, "w": size, "h": size}
        assert not frame["trimmed"] and not frame["rotated"]
        assert frame["sourceSize"] == {"w": size, "h": size}
        assert frame["spriteSourceSize"] == {"x": 0, "y": 0, "w": size, "h": size}, "Unexpected source offset"
        assert frame["duration"] == item["duration_ms"]
        assert tags[item["name"]] == (index, index), "Tag was extended when later frames were added"
        with Image.open(root / item["file"]) as source:
            raw = source.convert("RGBA")
        assert raw.size == (size, size)
        cell = atlas.crop((x, y, x + size, y + size))
        if views == 8:
            angle = (index // columns) * math.pi / 4
            expected = (math.cos(angle), math.sin(angle))
            assert math.dist(item["projected_heading"], expected) < 1e-5, "Facing does not project to its screen direction"
            assert item["name"] == poses[index % columns] + "." + ["e", "ne", "n", "nw", "w", "sw", "s", "se"][index // columns]
        assert raw.tobytes() == cell.tobytes(), "Import/export changed raw RGBA pixels"
        alpha = cell.getchannel("A")
        assert set(alpha.tobytes()) == {0, 255}, "AA or missing transparency"
        box = alpha.getbbox()
        assert box and box[0] > 0 and box[1] > 0 and box[2] < size and box[3] < size, "Clipping or blank pose"
        decoded.append(cell.tobytes())
        stats.append({"pose": item["name"], "bounds": box, "visible_height": box[3] - box[1], "duration_ms": frame["duration"]})
    assert len(set(decoded)) == len(frames), "Poses or directions did not change"
    return stats, decoded, metadata


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--preserve", type=Path, help="Assert every named pose from an older atlas is unchanged")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--density-reference", type=Path, help="Compare physical framing and timing across different cell densities")
    args = parser.parse_args()
    stats, pixels, metadata = validate(args.directory)
    if args.preserve:
        old_stats, old_pixels, _ = validate(args.preserve)
        by_name = {item["pose"]: data for item, data in zip(stats, pixels)}
        assert all(by_name[item["pose"]] == data for item, data in zip(old_stats, old_pixels)), "Existing named poses changed"
    if args.reference:
        _, reference_pixels, reference_metadata = validate(args.reference)
        assert pixels == reference_pixels, "Re-rendered pose pixels differ"
        assert metadata["frames"] == reference_metadata["frames"]
        assert metadata["meta"]["frameTags"] == reference_metadata["meta"]["frameTags"]
    result = {"passed": True, "raw_rgba_preserved": True, "reproduced": bool(args.reference), "poses": stats}
    if args.density_reference:
        old_stats, _, _ = validate(args.density_reference)
        old = json.loads((args.density_reference / "pose-contract.json").read_text(encoding="utf-8"))
        new = json.loads((args.directory / "pose-contract.json").read_text(encoding="utf-8"))
        assert old["frames"] == new["frames"], "Pose, direction or timing changed, not only density"
        assert old["pitch_degrees"] == new["pitch_degrees"]
        assert old["cell"] != new["cell"], "Not a different density"
        errors = []
        for a, b in zip(old_stats, stats):
            for edge in range(4):
                world_a = (a["bounds"][edge] - old["pivot"][edge % 2]) * old["cm_per_pixel"]
                world_b = (b["bounds"][edge] - new["pivot"][edge % 2]) * new["cm_per_pixel"]
                errors.append(abs(world_a - world_b))
        tolerance = max(old["cm_per_pixel"], new["cm_per_pixel"])
        assert max(errors) <= tolerance + 1e-6, "Silhouette framing changed beyond one coarse pixel"
        result["density_comparison"] = {"same_pose_direction_timing": True, "cell_span_cm": 208,
            "max_boundary_difference_cm": max(errors), "boundary_tolerance_cm": tolerance,
            "source_idle_height_px": [min(p["visible_height"] for p in old_stats if p["pose"].startswith("idle")),
                                      max(p["visible_height"] for p in old_stats if p["pose"].startswith("idle"))],
            "target_idle_height_px": [min(p["visible_height"] for p in stats if p["pose"].startswith("idle")),
                                      max(p["visible_height"] for p in stats if p["pose"].startswith("idle"))],
            "limits": "Raster bounds and matching metadata, not independent proof of identical scene geometry or final art quality."}
    if args.report:
        args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"POSE_VALIDATION_PASS {len(stats)} poses; {args.report}")
    else:
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
