#!/usr/bin/env python3
"""Load-time report from a tools/load_profile.sh log (REQ / LOAD / DONE lines).

Maps each request's LBA to its file by walking the ISO9660 tree of the MODE2 disc image
(build/game.bin, 2352-byte sectors) and prints, per load: requests, sectors, time inside CD
requests, load time, and the files that cost the most. With --order OUT it also writes the
disc files in the order the game first opens them (OPEN lines), which tools/disc/build_disc.py
uses to lay the disc out in load order.
Usage: load_profile.py LOG [DISC.bin] [--order OUT.txt [--iso-root build/iso]]
"""
import collections, re, struct, sys


def disc_files(path):
    f = open(path, 'rb')

    def sec(lba):
        f.seek(lba * 2352 + 24)
        return f.read(2048)

    files = []

    def walk(lba, size, prefix):
        for s in range((size + 2047) // 2048):
            d, o = sec(lba + s), 0
            while o < 2048 and d[o]:
                ln = d[o]
                el, sz = struct.unpack_from('<I', d, o + 2)[0], struct.unpack_from('<I', d, o + 10)[0]
                nm = d[o + 33:o + 33 + d[o + 32]]
                if nm not in (b'\0', b'\1'):
                    name = prefix + nm.decode(errors='replace').split(';')[0]
                    if d[o + 25] & 2:
                        walk(el, sz, name + '/')
                    else:
                        files.append((el, max(1, (sz + 2047) // 2048), name))
                o += ln

    pvd = sec(16)
    walk(struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0], '/')
    return sorted(files)


def write_order(log, out, iso_root):
    import os
    disc = {}
    for d, _, names in os.walk(iso_root):
        for n in names:
            rel = os.path.relpath(os.path.join(d, n), iso_root)
            disc[rel.lower()] = rel
    # Small files go where they are first opened; files bigger than a read-ahead window (64 KB)
    # where they are last opened: the tile set is opened for its header during the stage load
    # but read in bulk a frame later, next to BGStrips.bin.
    first, last, unknown = {}, {}, []
    for i, l in enumerate(log):
        if l.startswith('PRELOAD '):  # PS1FsPreloadDir reads the whole directory, in name order
            d = l[8:].strip().strip('/').lower() + '/'
            paths = sorted(k for k in disc if k.startswith(d) and '/' not in k[len(d):])
        elif l.startswith('OPEN '):
            paths = [l[5:].strip().lstrip('/').lower()]
        else:
            continue
        for j, p in enumerate(paths):
            if p not in disc:
                if p not in unknown:
                    unknown.append(p)
                continue
            first.setdefault(p, (i, j))
            last[p] = (i, j)
    big = lambda p: os.path.getsize(os.path.join(iso_root, disc[p])) > 64 * 1024
    order = [disc[p] for p in sorted(first, key=lambda p: last[p] if big(p) else first[p])]
    with open(out, 'w') as f:
        f.write('# Disc files in the order the game loads them (tools/load_profile.sh natural --order):\n'
                '# small files at their first open, files > 64 KB at their last open.\n'
                '# tools/disc/build_disc.py lays these out first, in this order; then the other data files;\n'
                '# CD-XA / STR streams last. Regenerate after changing what the game loads.\n')
        f.write('\n'.join(order) + '\n')
    print('order: %d files -> %s%s' % (len(order), out, (' | not on disc: ' + ', '.join(unknown)) if unknown else ''))


def main():
    args = sys.argv[1:]
    order_out = iso_root = None
    if '--order' in args:
        i = args.index('--order'); order_out = args[i + 1]; del args[i:i + 2]
    iso_root = 'build/iso'
    if '--iso-root' in args:
        i = args.index('--iso-root'); iso_root = args[i + 1]; del args[i:i + 2]
    log = open(args[0]).read().split('\n')
    if order_out:
        write_order(log, order_out, iso_root)
    files = disc_files(args[1] if len(args) > 1 else 'build/game.bin')

    def which(lba):
        for el, n, name in files:
            if el <= lba < el + n:
                return name
        return '(directory/system)'

    loads, cur = [], collections.OrderedDict()
    reqs = []
    for l in log:
        m = re.match(r'REQ lba=(\d+) n=(\d+) us=(\d+)', l)
        if m:
            reqs.append((int(m[1]), int(m[2]), int(m[3])))
            continue
        m = re.match(r'LOAD us=(\d+) frame=(\d+).*headroom=(\d+)', l)
        if m:
            loads.append((int(m[1]), int(m[2]), int(m[3]), reqs))
            reqs = []
        if l.startswith('DONE'):
            print(l)
    if reqs:
        loads.append((0, -1, 0, reqs))
    T = [0, 0, 0, 0]
    for i, (us, frame, headroom, rq) in enumerate(loads):
        n, s, t = len(rq), sum(r[1] for r in rq), sum(r[2] for r in rq)
        T = [T[0] + n, T[1] + s, T[2] + t, T[3] + us]
        print('load %d (frame %d): %.2f s | %d requests, %d sectors, %.2f s in CD requests | heap headroom %d' % (
            i + 1, frame, us / 1e6, n, s, t / 1e6, headroom) if frame >= 0 else
            'after the last load: %d requests, %d sectors, %.2f s' % (n, s, t / 1e6))
        per = collections.defaultdict(lambda: [0, 0, 0])
        for lba, cnt, u in rq:
            p = per[which(lba)]
            p[0] += 1; p[1] += cnt; p[2] += u
        for name, (c, sc, u) in sorted(per.items(), key=lambda kv: -kv[1][2])[:8]:
            print('    %-52s %3d req %4d sect %6.2f s' % (name[-52:], c, sc, u / 1e6))
    print('TOTAL: loads %.2f s, %d requests, %d sectors, %.2f s in CD requests' % (T[3] / 1e6, T[0], T[1], T[2] / 1e6))


if __name__ == '__main__':
    main()
