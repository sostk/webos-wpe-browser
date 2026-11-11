#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Resolve tools/tv-sampler samples on the PC.  report.py OUT_DIR LIBDIR [FROM_MS TO_MS]
# LIBDIR holds unstripped copies by file name (e.g. libWPEWebKit-2.0.so.1); others resolve to their library name.
# Self = the sampled pc. Inclusive = pc, lr and every stack word that points into code (a heuristic: stale return
# addresses and function pointers on the stack add some noise).
import collections, os, struct, subprocess, sys
out, libdir = sys.argv[1], sys.argv[2]
t_from = float(sys.argv[3]) if len(sys.argv) > 3 else 0
t_to = float(sys.argv[4]) if len(sys.argv) > 4 else 1e12
addr2line = os.path.expanduser('~/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-addr2line')
maps = []
for line in open(os.path.join(out, 'maps')):
    p = line.split()
    if len(p) < 6 or 'x' not in p[1]: continue
    a, b = (int(x, 16) for x in p[0].split('-'))
    maps.append((a, b, int(p[2], 16), p[5]))
def where(addr):
    for a, b, off, path in maps:
        if a <= addr < b: return path, addr - a + off
    return None
samples = []
data = open(os.path.join(out, 'samples.bin'), 'rb').read(); i = 0
while i + 28 <= len(data):
    magic, t, pc, lr, sp, cpsr, n = struct.unpack_from('<7I', data, i); i += 28
    if magic != 0x53414d50: break
    stack = data[i:i + n]; i += n
    if not (t_from <= t <= t_to): continue
    words = struct.unpack_from('<%dI' % (len(stack) // 4), stack)
    samples.append((pc, lr, words))
# collect addresses per library
want = collections.defaultdict(set)
def note(addr, ret):
    w = where(addr)
    if not w: return None
    path, off = w
    key = (os.path.basename(path), off - (2 if ret else 0))  # a return address points after the call
    want[key[0]].add(key[1]); return key
per = []
for pc, lr, words in samples:
    keys = [note(pc & ~1, False)]
    keys += [note(lr & ~1, True)] + [note(x & ~1, True) for x in words]
    per.append([k for k in keys if k])
names = {}
for lib, offs in want.items():
    p = os.path.join(libdir, lib)
    if not os.path.exists(p):
        for o in offs: names[(lib, o)] = lib
        continue
    offs = sorted(offs)
    res = subprocess.run([addr2line, '-f', '-C', '-e', p] + ['0x%x' % o for o in offs], capture_output=True, text=True).stdout.split('\n')
    for j, o in enumerate(offs):
        fn = res[2 * j] if 2 * j < len(res) else '??'
        names[(lib, o)] = fn if fn != '??' else lib + '+0x%x' % o
def short(s):
    s = s.replace('WebCore::', '').replace('WTF::', '').replace('JSC::', 'JSC:')
    return s if len(s) < 140 else s[:137] + '...'
self_c = collections.Counter(); incl = collections.Counter(); libs = collections.Counter()
for keys in per:
    if not keys: continue
    self_c[names[keys[0]]] += 1; libs[keys[0][0]] += 1
    for k in set(names[k] for k in keys): incl[k] += 1
n = len(per)
print('samples %d (%.0f..%.0f ms)' % (n, t_from, min(t_to, 1e9)))
print('--- self, by library'); [print('%5.1f%%  %s' % (100.0 * v / n, k)) for k, v in libs.most_common(8)]
print('--- self (where the thread was)'); [print('%5.1f%%  %s' % (100.0 * v / n, short(k))) for k, v in self_c.most_common(400)]
print('--- inclusive (on the stack)'); [print('%5.1f%%  %s' % (100.0 * v / n, short(k))) for k, v in incl.most_common(70)]
