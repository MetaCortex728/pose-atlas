"""Deterministic camera guides, not a 2D-to-3D reconstruction or art approval."""
import json
import math
from pathlib import Path

DEFAULT_PROFILE = Path(__file__).resolve().parents[1] / 'assets/dan/quarter-view-profile.json'
HEADINGS = ('e', 'ne', 'n', 'nw', 'w', 'sw', 's', 'se')


def load_profile(path=DEFAULT_PROFILE):
    p = json.loads(Path(path).read_text(encoding='utf-8'))
    if p['schema'] != 1 or p['projection'] != 'ORTHO':
        raise ValueError('Expected orthographic schema 1')
    if p['character_forward'] != '-Y' or p['world_up'] != '+Z':
        raise ValueError('Unsupported model axes')
    if tuple(p['screen_headings']) != HEADINGS:
        raise ValueError('Unexpected heading order')
    for name in ('elevation_degrees', 'base_azimuth_degrees', 'camera_distance_m', 'meters_per_pixel'):
        if not math.isfinite(p[name]):
            raise ValueError('Non-finite '+name)
    if not 0 < p['elevation_degrees'] < 90 or min(p['meters_per_pixel'], p['camera_distance_m']) <= 0:
        raise ValueError('Degenerate camera')
    if p['cell_pixels'] != 256:
        raise ValueError('Current sprite export contract requires 256px cells')
    if len(p['ground_pivot_pixels']) != 2 or not all(0 < v < 256 for v in p['ground_pivot_pixels']):
        raise ValueError('Invalid ground pivot')
    return p


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def rotate_z(v, angle):
    c, s = math.cos(angle), math.sin(angle)
    return (c*v[0]-s*v[1], s*v[0]+c*v[1], v[2])


def camera_guide(profile, direction):
    """Rotate camera around unchanged -Y-facing model; headings are SCREEN directions."""
    index = HEADINGS.index(direction)
    pitch, az = map(math.radians, (profile['elevation_degrees'], profile['base_azimuth_degrees']))
    cp, sp, ca, sa = math.cos(pitch), math.sin(pitch), math.cos(az), math.sin(az)
    toward = (cp*ca, cp*sa, sp)
    right, up = (-sa, ca, 0), (-sp*ca, -sp*sa, cp)
    angle = index*math.pi/4
    # Invert the ground-plane foreshortening before choosing the model heading.
    heading = tuple(right[i]*math.cos(angle)+up[i]*math.sin(angle)/(sp*sp) for i in (0, 1))
    rotation = -(math.atan2(heading[1], heading[0])+math.pi/2)
    toward, right, up = [rotate_z(v, rotation) for v in (toward, right, up)]
    cell, mpp = profile['cell_pixels'], profile['meters_per_pixel']
    px, py = profile['ground_pivot_pixels']
    offset_x, offset_y = (cell/2-px)*mpp, (py-cell/2)*mpp
    position = tuple(toward[i]*profile['camera_distance_m']+right[i]*offset_x+up[i]*offset_y for i in range(3))
    return {'direction': direction, 'toward': toward, 'right': right, 'up': up,
            'position': position, 'ortho_scale_m': cell*mpp,
            'camera_rotation_z_degrees': math.degrees(rotation),
            'vertical_projection_factor': cp, 'ground_depth_factor': sp}


def project(profile, guide, point):
    """Pixel coordinates relative to the declared ground pivot; Y points down."""
    px, py = profile['ground_pivot_pixels']
    mpp = profile['meters_per_pixel']
    return (px+dot(point, guide['right'])/mpp, py-dot(point, guide['up'])/mpp)
