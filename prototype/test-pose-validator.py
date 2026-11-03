"""Reject malformed export metadata without touching the original artwork."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("pose_check", Path(__file__).with_name("check-pose-pipeline.py"))
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)
ROOT = Path(__file__).resolve().parents[1] / "assets/prototypes/pose-locomotion"
GAIT = ROOT.with_name("shina-gait-atlas-v1")
RUN = ROOT.with_name("shina-run-atlas-v2")
ATTACK = ROOT.with_name("shina-attack-atlas-v1")
ENTRY = ROOT.with_name("shina-entry-atlas-v1")


class PoseValidatorTests(unittest.TestCase):
    def test_movement_entry_atlas(self):
        self.assertEqual(len(check.validate(ENTRY)[0]), 456)

    def test_entry_rejects_incomplete_contract(self):
        original = json.loads((ENTRY / "pose-contract.json").read_text(encoding="utf-8"))
        read_text = Path.read_text
        for changed in (dict(original, schema=6), dict(original, pose_names=original["pose_names"][:-1]),
                        dict(original, locomotion={"pose_count":48,"cycle_cm":240})):
            def read(path, *args, **kwargs):
                return json.dumps(changed) if path == ENTRY / "pose-contract.json" else read_text(path, *args, **kwargs)
            with patch.object(Path, "read_text", read), self.assertRaises(AssertionError):
                check.validate(ENTRY)

    def test_entry_preserves_earlier_color_and_normal_pixels(self):
        from PIL import Image
        contract = json.loads((ATTACK / "pose-contract.json").read_text(encoding="utf-8"))
        for frame in contract["frames"]:
            for prefix in ("", "normals/"):
                name = prefix + frame["file"]
                with Image.open(ATTACK / name) as a, Image.open(ENTRY / name) as b:
                    self.assertEqual(a.convert("RGBA").tobytes(), b.convert("RGBA").tobytes(), name)

    def test_baseline(self):
        self.assertEqual(len(check.validate(ROOT)[0]), 72)

    def test_eight_pose_gait(self):
        self.assertEqual(len(check.validate(GAIT)[0]), 104)

    def test_sixteen_pose_running(self):
        self.assertEqual(len(check.validate(RUN)[0]), 168)

    def test_attack_transition_atlas(self):
        self.assertEqual(len(check.validate(ATTACK)[0]), 200)

    def test_attack_rejects_incomplete_contract(self):
        original = json.loads((ATTACK / "pose-contract.json").read_text(encoding="utf-8"))
        read_text = Path.read_text
        for changed in (dict(original, schema=5), dict(original, pose_names=original["pose_names"][:-1]),
                        dict(original, locomotion={"pose_count":20,"cycle_cm":240})):
            def read(path, *args, **kwargs):
                return json.dumps(changed) if path == ATTACK / "pose-contract.json" else read_text(path, *args, **kwargs)
            with patch.object(Path, "read_text", read), self.assertRaises(AssertionError):
                check.validate(ATTACK)

    def test_attack_keeps_all_old_color_and_normal_pixels(self):
        from PIL import Image
        contract = json.loads((RUN / "pose-contract.json").read_text(encoding="utf-8"))
        for frame in contract["frames"]:
            for prefix in ("", "normals/"):
                name = prefix + frame["file"]
                with Image.open(RUN / name) as a, Image.open(ATTACK / name) as b:
                    self.assertEqual(a.convert("RGBA").tobytes(), b.convert("RGBA").tobytes(), name)

    def test_running_requires_matching_schema_and_calibration(self):
        original = json.loads((RUN / "pose-contract.json").read_text(encoding="utf-8"))
        read_text = Path.read_text
        for changed in (dict(original, schema=4), dict(original, locomotion={"pose_count":8,"cycle_cm":128}),
                        dict(original, locomotion={"pose_count":16,"cycle_cm":False})):
            def read(path, *args, **kwargs):
                return json.dumps(changed) if path == RUN / "pose-contract.json" else read_text(path, *args, **kwargs)
            with patch.object(Path, "read_text", read), self.assertRaises(AssertionError):
                check.validate(RUN)

    def test_running_keeps_action_pixels(self):
        from PIL import Image
        for direction in ("e", "ne", "n", "nw", "w", "sw", "s", "se"):
            for pose in ("idle", "windup", "impact", "recover", "dash"):
                for prefix in ("", "normals/"):
                    name = f"{prefix}{pose}.{direction}.png"
                    with Image.open(GAIT / name) as a, Image.open(RUN / name) as b:
                        a, b = a.convert("RGBA"), b.convert("RGBA")
                        self.assertIsNone(a.getchannel("A").crop((0, 0, 96, 6)).getbbox(), name)
                        self.assertIsNone(b.getchannel("A").crop((0, 90, 96, 96)).getbbox(), name)
                        self.assertEqual(a.crop((0, 6, 96, 96)).tobytes(), b.crop((0, 0, 96, 90)).tobytes(), name)

    def test_bad_gait_calibration(self):
        original = json.loads((GAIT / "pose-contract.json").read_text(encoding="utf-8"))
        read_text = Path.read_text
        for field, value in (("pose_count", 4), ("cycle_cm", 0), ("cycle_cm", -1),
                             ("cycle_cm", 1001), ("cycle_cm", True), ("cycle_cm", float("nan"))):
            with self.subTest(field=field, value=value):
                changed = copy.deepcopy(original)
                changed["locomotion"][field] = value
                def read(path, *args, **kwargs):
                    return json.dumps(changed) if path == GAIT / "pose-contract.json" else read_text(path, *args, **kwargs)
                with patch.object(Path, "read_text", read), self.assertRaises(AssertionError):
                    check.validate(GAIT)

    def test_action_pixels_preserved(self):
        from PIL import Image
        # Keep the independent pre-gait baseline packed, without the retired art study.
        old = Path(__file__).parent / "fixtures/gait-action-baseline"
        hashes = {
            "pose-proxy.png": "ac626033593b9f79b20db1f85868cc9544b9fb0ed5037e51b495f2f3c15a90d9",
            "normal-proxy.png": "ef5e1a67095d6243a10930a47bc05a01fd4bcf35151af109c2e31ac66720acb9",
            "pose-proxy.json": "0b85f1e0bbfcf55dc964214226fe75a2efc8d0bd233f363c91d516ed5faccd67",
        }
        for name, expected in hashes.items():
            self.assertEqual(hashlib.sha256((old / name).read_bytes()).hexdigest(), expected, name)
        meta = json.loads((old / "pose-proxy.json").read_text(encoding="utf-8"))
        tags = {tag["name"]: tag["from"] for tag in meta["meta"]["frameTags"]}
        for atlas_name, prefix in (("pose-proxy.png", ""), ("normal-proxy.png", "normals/")):
            with Image.open(old / atlas_name) as source:
                atlas = source.convert("RGBA")
                for direction in ("e", "ne", "n", "nw", "w", "sw", "s", "se"):
                    for pose in ("idle", "windup", "impact", "recover", "dash"):
                        name = f"{pose}.{direction}"
                        frame = meta["frames"][tags[name]]["frame"]
                        bounds = (frame["x"], frame["y"], frame["x"] + frame["w"], frame["y"] + frame["h"])
                        with Image.open(GAIT / f"{prefix}{name}.png") as image:
                            self.assertEqual(atlas.crop(bounds).tobytes(), image.convert("RGBA").tobytes(), name)

    def test_bad_metadata(self):
        original = json.loads((ROOT / "pose-proxy.json").read_text(encoding="utf-8"))
        mutations = {
            "extra slice": lambda data: data["meta"]["slices"].append(copy.deepcopy(data["meta"]["slices"][0])),
            "wrong slice name": lambda data: data["meta"]["slices"][0].update(name="wrong"),
            "late pivot key": lambda data: data["meta"]["slices"][0]["keys"][0].update(frame=1),
            "source offset": lambda data: data["frames"][0]["spriteSourceSize"].update(x=1),
            "different duration": lambda data: data["frames"][0].update(duration=151),
        }
        read_text = Path.read_text
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                changed = copy.deepcopy(original)
                mutate(changed)
                def read(path, *args, **kwargs):
                    return json.dumps(changed) if path == ROOT / "pose-proxy.json" else read_text(path, *args, **kwargs)
                with patch.object(Path, "read_text", read), self.assertRaises(AssertionError):
                    check.validate(ROOT)


if __name__ == "__main__":
    unittest.main()
