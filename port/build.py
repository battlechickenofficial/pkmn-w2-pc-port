#!/usr/bin/env python3
"""Build the Windows port: recompile the ARM9 modules to C, then compile and link them with MSVC.

Usage: python port/build.py [version] [module ...]    (default: w2, all modules) -> build/<version>/port/b2w2.exe
Needs: tools/recomp built (cargo build --release in tools/recomp), Visual Studio 2022 with the C++ workload.
"""
import os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = sys.argv[1] if len(sys.argv) > 1 else 'w2'
OUT = ROOT / 'build' / VERSION / 'port'
GEN = OUT / 'gen'
OBJ = OUT / 'obj'
RUNTIME = ROOT / 'port' / 'runtime'
RECOMP = ROOT / 'tools' / 'recomp' / 'target' / 'release' / 'recomp.exe'
VCVARS = Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')

CFLAGS = ['/nologo', '/c', '/O2', '/utf-8', '/W3', '/D_CRT_SECURE_NO_WARNINGS', '/wd4102', '/wd4146', '/wd4244',
          '/MP1', f'/I{RUNTIME}', f'/I{GEN}']
RUNTIME_SOURCES = ['runtime.c', 'sched.c', 'arm7.c', 'card.c', 'snd.c', 'render.c', 'gpu3d.c', 'frontend.c',
                   'saves.c', 'font.c', 'titlemenu.c', 'topscreen.c']


def msvc_env():
    out = subprocess.run(f'"{VCVARS}" >nul && set', shell=True, capture_output=True, text=True, check=True).stdout
    env = dict(line.split('=', 1) for line in out.splitlines() if '=' in line)
    path = next(v for k, v in env.items() if k.upper() == 'PATH')
    # CreateProcess looks programs up in our own PATH, not the child's, so resolve them here
    return env, shutil.which('cl', path=path), shutil.which('link', path=path)


def main():
    if not RECOMP.exists():
        sys.exit(f'{RECOMP} missing: run cargo build --release in tools/recomp')
    modules = sys.argv[2:] or ['all']
    subprocess.run([str(RECOMP), VERSION, str(GEN), *modules], cwd=ROOT, check=True)
    env, cl, link = msvc_env()
    OBJ.mkdir(parents=True, exist_ok=True)
    sources = [RUNTIME / n for n in RUNTIME_SOURCES if (RUNTIME / n).exists()]
    sources += [GEN / n for n in ['tables.c', 'overlay_data.c', *(GEN / 'files.txt').read_text().split()]]
    headers = max(h.stat().st_mtime for h in RUNTIME.glob('*.h'))

    def compile(src):
        obj = OBJ / (src.stem + '.obj')
        if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, headers):
            return src, 0, ''
        p = subprocess.run([cl, *CFLAGS, str(src), f'/Fo{obj}'], env=env, capture_output=True, text=True)
        return src, p.returncode, p.stdout + p.stderr

    failed = False
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        for src, rc, log in pool.map(compile, sources):
            lines = [l for l in log.splitlines() if l.strip() and l.strip() != src.name]
            if rc or any('error' in l or 'warning' in l for l in lines):
                print(f'{src.name}:', *lines[:20], sep='\n  ')
            failed |= rc != 0
    if failed:
        sys.exit(1)
    exe = OUT / 'b2w2.exe'
    objs = [str(OBJ / (s.stem + '.obj')) for s in sources]
    p = subprocess.run([link, '/nologo', '/STACK:268435456', f'/OUT:{exe}', *objs, 'user32.lib', 'gdi32.lib',
                        'winmm.lib'], env=env, capture_output=True, text=True)
    if p.returncode:
        sys.exit(p.stdout + p.stderr)
    print(f'built {exe}')


if __name__ == '__main__':
    main()
