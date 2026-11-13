#!/usr/bin/env python3
"""Compose upstream media, configurable rain and the fish without an engine fork."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def split_defaults(source, path):
    first, separator, body = source.partition('\n')
    if not separator or not first.startswith('// defaults:'):
        raise ValueError(f'Expected shader defaults on the first line: {path}')
    defaults = json.loads(first.removeprefix('// defaults:'))
    if not isinstance(defaults, dict):
        raise ValueError(f'Shader defaults must be an object: {path}')
    return defaults, body


def compose(lib, output):
    media_path = Path(lib) / 'media.glsl'
    rain_path = ROOT / 'shaders/matrix-background.glsl'
    defaults, media = split_defaults(media_path.read_text(), media_path)
    rain_defaults, rain = split_defaults(rain_path.read_text(), rain_path)
    defaults.update(rain_defaults)
    fish = (ROOT / 'shaders/fish-hover.glsl').read_text()
    shader = ('// defaults: ' + json.dumps(defaults, separators=(',', ':'))
              + '\n#define cell rawMediaCell\n' + media
              + '\n#undef cell\n' + rain + '\n' + fish)
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(shader)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lib', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    compose(args.lib, args.output)


if __name__ == '__main__':
    main()
