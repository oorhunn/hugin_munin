#!/usr/bin/env python3
"""Check the app's minimum OS metadata; runtime checks remain a separate step."""
import argparse
import pathlib
import re
import subprocess


def version(value):
    return tuple((list(map(int, value.split('.'))) + [0, 0])[:3])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=pathlib.Path)
    args = parser.parse_args()
    for name in ('HuginMunin', 'hm-service', 'hm-mcp', 'hm-run'):
        binary = args.app / 'Contents' / 'MacOS' / name
        output = subprocess.check_output(['/usr/bin/vtool', '-show-build', str(binary)], text=True)
        minimums = re.findall(r'^\s*minos\s+(\d+(?:\.\d+)*)\s*$', output, re.MULTILINE)
        if not minimums or any(version(v) > version('14.0') for v in minimums):
            raise SystemExit(f'{name}: missing metadata or minimum OS newer than macOS 14: {minimums}')
        print(f'{name}: minimum OS {", ".join(minimums)}')
    print('Metadata passed. This does not establish runtime compatibility on macOS 14.')


if __name__ == '__main__':
    main()
