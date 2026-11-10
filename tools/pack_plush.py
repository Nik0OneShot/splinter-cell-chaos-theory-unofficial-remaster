"""Packs the Niko plush (the CLUSTERFUCK unlock) into System\\SCCTRemaster_Award.pak.

Input: a folder with a glTF model (scene.gltf + its .bin and texture) and a meow sound (any audio file ffmpeg reads).
The folder isn't in the repository (references\\niko_plush, local only): the model is a Sketchfab download whose
licence allows using it in a project but not handing the file on as it is, so only this packed form ships.

Output (little-endian):
    "NIKO" u32 version=1
    u32 vertexCount, u32 indexCount
    vertexCount x { float pos[3]; float normal[3]; float uv[2] }   Unreal units: X forward, Y right, Z up, centred
    indexCount x u16                                                 triangle list
    u32 width, u32 height, width*height x BGRA                     the texture
    u32 sampleRate, u32 sampleCount, sampleCount x s16              the meow, mono
Usage: pack_plush.py <folder> <out.pak> [height in Unreal units, default 26]
"""
import json, os, struct, subprocess, sys

from PIL import Image


def mat_mul(a, b):   # 4x4 column-major lists
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def node_matrix(n):
    if 'matrix' in n:
        return list(n['matrix'])
    t = n.get('translation', [0, 0, 0]); q = n.get('rotation', [0, 0, 0, 1]); s = n.get('scale', [1, 1, 1])
    x, y, z, w = q
    r = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
         0, 0, 0, 1]
    for c in range(3):
        for rr in range(3):
            r[c * 4 + rr] *= s[c]
    r[12], r[13], r[14] = t
    return r


def main():
    folder, out = sys.argv[1], sys.argv[2]
    height = float(sys.argv[3]) if len(sys.argv) > 3 else 26.0
    gltf_path = next(os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith('.gltf'))
    g = json.load(open(gltf_path, encoding='utf-8'))
    base = os.path.dirname(gltf_path)
    bins = [open(os.path.join(base, b['uri']), 'rb').read() for b in g['buffers']]

    def accessor(i):
        a = g['accessors'][i]
        v = g['bufferViews'][a['bufferView']]
        data = bins[v['buffer']]
        off = v.get('byteOffset', 0) + a.get('byteOffset', 0)
        comps = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        fmt = {5126: 'f', 5125: 'I', 5123: 'H', 5121: 'B'}[a['componentType']]
        size = struct.calcsize(fmt)
        stride = v.get('byteStride', size * comps)
        return [struct.unpack_from('<' + fmt * comps, data, off + i * stride) for i in range(a['count'])]

    # World matrix of every node.
    parents = {}
    for i, n in enumerate(g['nodes']):
        for c in n.get('children', []):
            parents[c] = i

    def world(i):
        m = node_matrix(g['nodes'][i])
        while i in parents:
            i = parents[i]
            m = mat_mul(node_matrix(g['nodes'][i]), m)
        return m

    verts, idx = [], []
    for ni, n in enumerate(g['nodes']):
        if 'mesh' not in n:
            continue
        m = world(ni)
        for prim in g['meshes'][n['mesh']]['primitives']:
            att = prim['attributes']
            pos = accessor(att['POSITION']); nor = accessor(att['NORMAL']); uv = accessor(att['TEXCOORD_0'])
            ind = [t[0] for t in accessor(prim['indices'])]
            start = len(verts)
            for p, q, t in zip(pos, nor, uv):
                wp = [sum(m[c * 4 + r] * (p[c] if c < 3 else 1.0) for c in range(4)) for r in range(3)]
                wn = [sum(m[c * 4 + r] * q[c] for c in range(3)) for r in range(3)]   # uniform scale: fine
                l = sum(x * x for x in wn) ** 0.5 or 1.0
                wn = [x / l for x in wn]
                # glTF (right-handed, Y up, front +Z) -> Unreal (left-handed, Z up, front +X).
                verts.append([(wp[2], -wp[0], wp[1]), (wn[2], -wn[0], wn[1]), t])
            idx += [start + i for i in ind]

    lo = [min(v[0][k] for v in verts) for k in range(3)]
    hi = [max(v[0][k] for v in verts) for k in range(3)]
    centre = [(lo[k] + hi[k]) / 2 for k in range(3)]
    scale = height / (hi[2] - lo[2])
    for v in verts:
        v[0] = tuple((v[0][k] - centre[k]) * scale for k in range(3))
    size = [(hi[k] - lo[k]) * scale for k in range(3)]

    # Texture: the base colour, 512x512 BGRA.
    mat = g['materials'][g['meshes'][0]['primitives'][0].get('material', 0)]
    tex = g['textures'][mat['pbrMetallicRoughness']['baseColorTexture']['index']]
    img = Image.open(os.path.join(base, g['images'][tex['source']]['uri'])).convert('RGBA').resize((512, 512), Image.LANCZOS)
    r, gch, b, a = img.split()
    bgra = Image.merge('RGBA', (b, gch, r, a)).tobytes()

    # The meow: mono 22050 Hz 16-bit.
    audio = next((os.path.join(folder, f) for f in sorted(os.listdir(folder))
                  if f.lower().endswith(('.mp3', '.wav', '.ogg', '.flac', '.m4a'))), None)
    if not audio:
        sys.exit('no meow sound (mp3/wav/ogg/flac/m4a) in ' + folder)
    pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', audio, '-ac', '1', '-ar', '22050', '-f', 's16le', '-'],
                         capture_output=True, check=True).stdout

    with open(out, 'wb') as f:
        f.write(b'NIKO' + struct.pack('<III', 1, len(verts), len(idx)))
        for p, q, t in verts:
            f.write(struct.pack('<8f', *p, *q, *t))
        f.write(struct.pack('<%dH' % len(idx), *idx))
        f.write(struct.pack('<II', 512, 512) + bgra)
        f.write(struct.pack('<II', 22050, len(pcm) // 2) + pcm[:len(pcm) // 2 * 2])
    print('Niko plush: %d vertices, %d triangles, %.1f x %.1f x %.1f units, meow %.2f s -> %s' %
          (len(verts), len(idx) // 3, size[0], size[1], size[2], len(pcm) / 2 / 22050, out))


if __name__ == '__main__':
    main()
