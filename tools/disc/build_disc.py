#!/usr/bin/env python3
"""Build the PS1 disc image with mkpsxiso (phase 5): build/game.bin + build/game.cue.

A MODE2/2352 BIN/CUE is needed for CD-XA music and STR files with XA audio (2336-byte mixed
sectors, which an hdiutil ISO can't carry). The image boots through the BIOS too:
SYSTEM.CNF (BOOT = cdrom:\\PSX.EXE;1) + PSX.EXE (the current rsdkv2.ps-exe) + the Sony
license sectors, taken from the workspace's LICENSE.DAT (never copied into the repo; build/ is
git-ignored).

Tree: every file under ISO_ROOT (build/iso: Data/**), macOS metadata skipped. `.xa` files and
`.str` files whose size is a whole number of 2336-byte sectors are `type="mixed"`; everything
else is data (Form 1, read by ps1/cdrom_fs.cpp as 2048-byte sectors).

Usage: build_disc.py [ISO_ROOT] [EXE] [LICENSE] [OUT_PREFIX]
  defaults: build/iso rsdkv2.ps-exe ../LICENSE.DAT build/game
"""
import os, subprocess, sys
from xml.sax.saxutils import quoteattr

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
MKPSXISO = os.path.join(REPO, '..', 'mkpsxiso-2.30-Darwin', 'bin', 'mkpsxiso')

SYSTEM_CNF = 'BOOT = cdrom:\\PSX.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n'


def file_type(path):
    ext = os.path.splitext(path)[1].lower()
    size = os.path.getsize(path)
    if ext == '.xa' or (ext == '.str' and size % 2336 == 0 and size % 2048 != 0):
        return 'mixed'
    return 'data'


def emit_dir(path, indent, out, counts):
    for name in sorted(os.listdir(path)):
        if name.startswith('.'):
            continue
        full = os.path.join(path, name)
        pad = '\t' * indent
        if os.path.isdir(full):
            out.append('%s<dir name=%s>' % (pad, quoteattr(name)))
            emit_dir(full, indent + 1, out, counts)
            out.append('%s</dir>' % pad)
        else:
            t = file_type(full)
            counts[t] = counts.get(t, 0) + 1
            out.append('%s<file name=%s type="%s" source=%s/>' % (pad, quoteattr(name), t, quoteattr(full)))


def main():
    args = sys.argv[1:]
    iso_root = os.path.abspath(args[0] if len(args) > 0 else os.path.join(REPO, 'build', 'iso'))
    exe = os.path.abspath(args[1] if len(args) > 1 else os.path.join(REPO, 'rsdkv2.ps-exe'))
    lic = os.path.abspath(args[2] if len(args) > 2 else os.path.join(REPO, '..', 'LICENSE.DAT'))
    out = os.path.abspath(args[3] if len(args) > 3 else os.path.join(REPO, 'build', 'game'))
    work = os.path.join(os.path.dirname(out), 'disc')
    os.makedirs(work, exist_ok=True)
    cnf = os.path.join(work, 'SYSTEM.CNF')
    open(cnf, 'w', newline='').write(SYSTEM_CNF)
    if not os.path.exists(lic):
        sys.exit('ERROR: license file %s not found' % lic)

    counts = {}
    tree = []
    tree.append('\t\t\t<file name="SYSTEM.CNF" source=%s/>' % quoteattr(cnf))
    tree.append('\t\t\t<file name="PSX.EXE" source=%s/>' % quoteattr(exe))
    for name in sorted(os.listdir(iso_root)):
        full = os.path.join(iso_root, name)
        if os.path.isdir(full):
            tree.append('\t\t\t<dir name=%s>' % quoteattr(name))
            emit_dir(full, 4, tree, counts)
            tree.append('\t\t\t</dir>')
    xml = '\n'.join([
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<iso_project image_name=%s cue_sheet=%s>' % (quoteattr(out + '.bin'), quoteattr(out + '.cue')),
        '\t<track type="data">',
        '\t\t<identifiers system="PLAYSTATION" application="PLAYSTATION" volume="RSDKV2PS1" volume_set="RSDKV2PS1"'
        ' publisher="RSDKV2PS1" data_preparer="MKPSXISO"/>',
        '\t\t<license file=%s/>' % quoteattr(lic),
        '\t\t<directory_tree>',
    ] + tree + [
        '\t\t</directory_tree>',
        '\t</track>',
        '</iso_project>',
        '',
    ])
    xml_path = os.path.join(work, 'disc.xml')
    open(xml_path, 'w').write(xml)
    subprocess.run([MKPSXISO, '-y', '-q', '-w', xml_path], check=True)
    size = os.path.getsize(out + '.bin')
    print('disc %s.bin/.cue: %.1f MB (%d sectors) | files: %s | license %s' % (
        out, size / 1048576, size // 2352, ', '.join('%s %d' % kv for kv in sorted(counts.items())), os.path.basename(lic)))


if __name__ == '__main__':
    main()
