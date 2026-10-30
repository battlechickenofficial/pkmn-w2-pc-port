#!/usr/bin/env python3
"""Run a check and touch a stamp file if it passes, so ninja works the same on Windows and POSIX.

  stamp.py <stamp> --sha1 <expected> <file>   pass if the file's SHA-1 matches
  stamp.py <stamp> <command...>               pass if the command exits 0
"""
import hashlib, subprocess, sys
from pathlib import Path

stamp, args = Path(sys.argv[1]), sys.argv[2:]
if args[0] == '--sha1':
    expected, path = args[1], args[2]
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    if h.hexdigest() != expected:
        sys.exit(f'{path}: SHA-1 {h.hexdigest()}, expected {expected}')
    print(f'{path}: OK')
elif subprocess.run(args).returncode != 0:
    sys.exit(1)
stamp.parent.mkdir(parents=True, exist_ok=True)
stamp.touch()
