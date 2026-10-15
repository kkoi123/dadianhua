#!/usr/bin/env python3
"""Stage only the web entry's fixed public files; exclude Finder metadata."""
import argparse, os, pathlib, shutil
NAMES=('index.html','app.css','app.js','events.js','sms-codec.js','communications.js','call-audio.js','audio.js','pcm-worklet.js')
p=argparse.ArgumentParser();p.add_argument('--source',type=pathlib.Path,required=True);p.add_argument('--dest',type=pathlib.Path,required=True);a=p.parse_args()
assert a.source.is_dir() and not a.source.is_symlink()
assert not a.dest.is_symlink();a.dest.mkdir(parents=True,exist_ok=True)
for name in NAMES:
    src=a.source/name;assert src.is_file() and not src.is_symlink(),name
    dest=a.dest/name;assert not dest.is_symlink(),name
    shutil.copy2(src,dest)
# Remove only generated Finder metadata from an older package. Never traverse
# symlink directories or delete source files, binaries or configuration.
for current,dirs,files in os.walk(a.dest,followlinks=False):
    for name in files:
        if name=='.DS_Store' or name.startswith('._'):(pathlib.Path(current)/name).unlink()
