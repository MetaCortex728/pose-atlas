#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the expected camera guides for the C++ port of the prototype maths.

Runs the prototype's own camera_guide and project (prototype/dan_projection.py)
on prototype/quarter-view-profile.json and writes every guide and the
projection of a few sample points as JSON. The C++ tests compare against this
file. With --check, compare instead of writing and exit 1 on any difference.

    python3 tests/fixtures/make_camera_fixture.py [--check]
"""
import argparse
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROTOTYPE = ROOT / 'prototype'
FIXTURE = Path(__file__).resolve().parent / 'camera_guides_quarter_view.json'

# Model-frame points (metres, forward -Y, up +Z): the pivot, head height,
# points off every axis, and a point below the ground.
POINTS = [
    [0.0, 0.0, 0.0],
    [0.0, 0.0, 1.8],
    [0.3, -0.2, 1.0],
    [-0.5, 0.4, 0.1],
    [1.0, 1.0, 1.0],
    [0.25, 0.75, -0.5],
]


def load_prototype():
    spec = importlib.util.spec_from_file_location('dan_projection', PROTOTYPE / 'dan_projection.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build():
    proj = load_prototype()
    profile = proj.load_profile(PROTOTYPE / 'quarter-view-profile.json')
    guides = []
    for direction in proj.HEADINGS:
        guide = proj.camera_guide(profile, direction)
        guides.append({
            'direction': guide['direction'],
            'toward': list(guide['toward']),
            'right': list(guide['right']),
            'up': list(guide['up']),
            'position': list(guide['position']),
            'ortho_scale_m': guide['ortho_scale_m'],
            'camera_rotation_z_degrees': guide['camera_rotation_z_degrees'],
            'vertical_projection_factor': guide['vertical_projection_factor'],
            'ground_depth_factor': guide['ground_depth_factor'],
            'projected': [list(proj.project(profile, guide, p)) for p in POINTS],
        })
    return {
        'generator': 'tests/fixtures/make_camera_fixture.py',
        'source': 'prototype/dan_projection.py on prototype/quarter-view-profile.json',
        'points': POINTS,
        'guides': guides,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', action='store_true', help='compare with the committed fixture')
    args = parser.parse_args()
    text = json.dumps(build(), indent=1) + '\n'
    if args.check:
        if FIXTURE.read_text(encoding='utf-8') != text:
            print(f'{FIXTURE} is out of date; rerun {Path(__file__).name}', file=sys.stderr)
            return 1
        print(f'{FIXTURE.name} matches the prototype')
        return 0
    FIXTURE.write_text(text, encoding='utf-8')
    print(f'wrote {FIXTURE}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
