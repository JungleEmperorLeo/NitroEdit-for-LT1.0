"""LithTech 1.0 (Blood 2 / Shogo) world .dat, version 56: full reader + writer.

File:  u32 version(56), u32 objectPos, u32 bspPos, u32 len + world info string,
       32 zero bytes, objects (at objectPos), world models (at bspPos),
       u32 (model count - 1) after the first model, 24 bytes world extents.
Model: u32 nextPos, 32 zero bytes, BSP, render data.
"""
import struct

class R:
    def __init__(s, b, p=0): s.b = b; s.p = p
    def take(s, n): v = s.b[s.p:s.p+n]; assert len(v) == n, 'eof'; s.p += n; return v
    def u(s, f):
        v = struct.unpack_from('<' + f, s.b, s.p); s.p += struct.calcsize('<' + f)
        return v if len(v) > 1 else v[0]
    def s16(s): return s.take(s.u('H'))

class W:
    def __init__(s): s.parts = []; s.n = 0
    def put(s, b): s.parts.append(b); s.n += len(b)
    def u(s, f, *v): s.put(struct.pack('<' + f, *v))
    def s16(s, b): s.u('H', len(b)); s.put(b)
    def bytes(s): return b''.join(s.parts)

def read_bsp(r):
    m = {}
    m['infoflags'] = r.u('I'); m['name'] = r.s16()
    (m['unk1'], np, npl, ns, nup, npo, nl, m['nverts'], m['vislistsize'], m['nleaflists'], nn, m['unk5']) = r.u('12I')
    m['minbox'] = r.u('3f'); m['maxbox'] = r.u('3f'); m['translation'] = r.u('3f')
    m['nameslen'], nt = r.u('2I')
    m['textures'] = []
    for i in range(nt):
        e = r.b.index(b'\0', r.p); m['textures'].append(r.b[r.p:e]); r.p = e + 1
    m['polycounts'] = [r.u('H') for _ in range(npo)]
    leafs = []
    for i in range(nl):
        L = {}
        k = r.u('H'); L['nlists'] = k
        if k == 0xFFFF: L['listindex'] = r.u('H'); L['lists'] = []
        else: L['lists'] = [(r.u('H'), r.s16()) for _ in range(k)]
        c = r.u('H'); L['polies'] = r.take(c * 4); L['f'] = r.u('f')
        leafs.append(L)
    m['leafs'] = leafs
    m['planes'] = [r.u('4f') for _ in range(npl)]
    surfs = []
    for i in range(ns):
        S = {}
        S['O'], S['P'], S['Q'], S['u1'], S['u2'], S['u3'] = [r.u('3f') for _ in range(6)]
        S['tex'] = r.u('H'); S['plane'] = r.u('I'); S['flags'] = r.u('I'); S['b4'] = r.take(4)
        S['useeffect'] = r.u('B')
        if S['useeffect']: S['effect'] = r.s16(); S['effectparam'] = r.s16()
        S['texflags'] = r.u('H')
        surfs.append(S)
    m['surfaces'] = surfs
    polys = []
    for i in range(npo):
        c = m['polycounts'][i]; nv = (c & 255) + (c >> 8)
        P = {}
        P['lmw'], P['lmh'], P['u1'], P['u2'], P['surface'] = r.u('HHIII')
        P['verts'] = [(r.u('H'), r.take(3)) for _ in range(nv)]
        polys.append(P)
    m['polies'] = polys
    m['nodes'] = [r.u('IIHii4f') for _ in range(nn)]
    m['userportals'] = [(r.s16(), r.u('I'), r.u('H'), r.u('3f'), r.u('3f')) for _ in range(nup)]
    m['points'] = [r.u('3f') for _ in range(np)]
    a, b, c = r.u('3I'); m['pb_dims'] = (a, b, c); m['pb_min'] = r.u('3f'); m['pb_max'] = r.u('3f')
    recs = []
    for i in range(a * b * c):
        n = r.u('H'); w = r.u('H'); recs.append((w, r.take(n * 6)))
    m['pb_records'] = recs
    m['rootnode'] = r.u('i')
    return m

def write_bsp(w, m):
    w.u('I', m['infoflags']); w.s16(m['name'])
    w.u('12I', m['unk1'], len(m['points']), len(m['planes']), len(m['surfaces']), len(m['userportals']),
        len(m['polies']), len(m['leafs']), m['nverts'], m['vislistsize'], m['nleaflists'], len(m['nodes']), m['unk5'])
    w.u('3f', *m['minbox']); w.u('3f', *m['maxbox']); w.u('3f', *m['translation'])
    w.u('2I', m['nameslen'], len(m['textures']))
    for t in m['textures']: w.put(t + b'\0')
    for c in m['polycounts']: w.u('H', c)
    for L in m['leafs']:
        w.u('H', L['nlists'])
        if L['nlists'] == 0xFFFF: w.u('H', L['listindex'])
        else:
            for pid, data in L['lists']: w.u('H', pid); w.s16(data)
        w.u('H', len(L['polies']) // 4); w.put(L['polies']); w.u('f', L['f'])
    for p in m['planes']: w.u('4f', *p)
    for S in m['surfaces']:
        for k in ('O', 'P', 'Q', 'u1', 'u2', 'u3'): w.u('3f', *S[k])
        w.u('H', S['tex']); w.u('I', S['plane']); w.u('I', S['flags']); w.put(S['b4'])
        w.u('B', S['useeffect'])
        if S['useeffect']: w.s16(S['effect']); w.s16(S['effectparam'])
        w.u('H', S['texflags'])
    for P in m['polies']:
        w.u('HHIII', P['lmw'], P['lmh'], P['u1'], P['u2'], P['surface'])
        for v, col in P['verts']: w.u('H', v); w.put(col)
    for n in m['nodes']: w.u('IIHii4f', *n)
    for name, a, b, c, d in m['userportals']: w.s16(name); w.u('I', a); w.u('H', b); w.u('3f', *c); w.u('3f', *d)
    for p in m['points']: w.u('3f', *p)
    w.u('3I', *m['pb_dims']); w.u('3f', *m['pb_min']); w.u('3f', *m['pb_max'])
    for wd, data in m['pb_records']: w.u('H', len(data) // 6); w.u('H', wd); w.put(data)
    w.u('i', m['rootnode'])

def read_render(r, m):
    rd = {}
    rd['u1'] = r.u('I'); rd['polyvecs'] = [r.u('3f') for _ in m['polies']]; rd['u2'] = r.u('I')
    lms = []
    for P in m['polies']:
        if m['surfaces'][P['surface']]['flags'] & 0x80:
            w_ = r.u('B'); h = r.u('B'); lms.append((w_, h, r.take(w_ * h * 2)))
        else: lms.append(None)
    rd['lightmaps'] = lms
    return rd

def write_render(w, m, rd):
    w.u('I', rd['u1'])
    for v in rd['polyvecs']: w.u('3f', *v)
    w.u('I', rd['u2'])
    for P, lm in zip(m['polies'], rd['lightmaps']):
        if m['surfaces'][P['surface']]['flags'] & 0x80:
            w.u('BB', lm[0], lm[1]); w.put(lm[2])

def read(fn):
    b = open(fn, 'rb').read(); r = R(b)
    d = {}
    d['version'], objpos, bsppos = r.u('3I')
    d['info'] = r.take(r.u('I')); d['pad'] = r.take(32)
    assert r.p == objpos
    objs = []
    for i in range(r.u('I')):
        dl = r.u('H'); s = r.p; cls = r.s16(); props = []
        for j in range(r.u('I')):
            name = r.s16(); code = r.u('B'); fl = r.u('I'); data = r.s16()
            props.append([name, code, fl, data])
        assert r.p == s + dl
        objs.append([cls, props])
    d['objects'] = objs
    assert r.p == bsppos
    models = []; n = None; i = 0
    while True:
        if i == 1: n = r.u('I') + 1
        if i > 0 and i >= n: break
        nxt = r.u('I'); z = r.take(32); m = read_bsp(r); m['zero32'] = z
        m['render'] = read_render(r, m)
        assert r.p == nxt, (i, r.p, nxt)
        models.append(m); i += 1
    d['models'] = models
    d['extents'] = r.u('6f')
    assert r.p == len(b)
    return d

def write(d):
    w = W()
    w.u('3I', d['version'], 0, 0)
    w.u('I', len(d['info'])); w.put(d['info']); w.put(d['pad'])
    objpos = w.n
    w.u('I', len(d['objects']))
    for cls, props in d['objects']:
        o = W(); o.s16(cls); o.u('I', len(props))
        for name, code, fl, data in props: o.s16(name); o.u('B', code); o.u('I', fl); o.s16(data)
        ob = o.bytes(); w.u('H', len(ob)); w.put(ob)
    bsppos = w.n
    for i, m in enumerate(d['models']):
        if i == 1: w.u('I', len(d['models']) - 1)
        mw = W(); mw.put(m['zero32']); write_bsp(mw, m); write_render(mw, m, m['render'])
        mb = mw.bytes(); w.u('I', w.n + 4 + len(mb)); w.put(mb)
    if len(d['models']) == 1: w.u('I', 0)
    w.u('6f', *d['extents'])
    out = bytearray(w.bytes()); struct.pack_into('<II', out, 4, objpos, bsppos)
    return bytes(out)

if __name__ == '__main__':
    import sys
    for fn in sys.argv[1:]:
        orig = open(fn, 'rb').read(); d = read(fn); out = write(d)
        print(fn, 'roundtrip', out == orig, len(orig), len(out))
