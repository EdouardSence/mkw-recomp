"""Check the warp matrix functions of the translator (LDSM_FN, HMMA_FN, MOVM_FN in sm86/translate/glsl/common.py) off
the GPU: the same per-lane formulas in numpy, fed as ptxas lays out an A x B product.

Why: the only programs that use them (ndsr's 30 compute shaders) are never requested by the game, so no run checks
them. A is loaded with LDSM.M88.4, B with LDSM.MT88.4 (as ldmatrix .trans), multiplied with HMMA.16816.F16 and
compared with A @ B; MOVM.16.MT88 must give the exact transpose. A change to one of those GLSL functions is repeated
here first.

Usage: uv run tools/shaders/mma_check.py   (prints the largest HMMA error, ~1e-3 from f16 rounding, and the MOVM result)
"""

import numpy as np

rng = np.random.default_rng(1)


def h(x):
    return np.float16(x).view(np.uint16).astype(np.uint32)


def f(u):
    return np.uint16(u & 0xFFFF).view(np.float16).astype(np.float32)


def pack(a, b):
    return int(h(a)) | (int(h(b)) << 16)


smem = np.zeros(4096, np.uint32)


def smem16(b):
    return (int(smem[b >> 2]) >> ((b & 2) * 8)) & 0xFFFF


def store16(b, v):
    w = int(smem[b >> 2])
    sh = (b & 2) * 8
    smem[b >> 2] = (w & ~(0xFFFF << sh)) | (int(v) << sh)


def ldsm(addr, i, trans):  # addr: per-lane list
    out = []
    for lane in range(32):
        r, q = lane >> 2, lane & 3
        p = addr[i * 8 + r]
        c0 = addr[i * 8 + 2 * q]
        c1 = addr[i * 8 + 2 * q + 1]
        out.append(smem16(c0 + 2 * r) | (smem16(c1 + 2 * r) << 16) if trans else int(smem[(p >> 2) + q]))
    return out


def hmma(a0, a1, a2, a3, b0, b1, c0, c1):
    d0 = []
    d1 = []
    for lane in range(32):
        g, t = lane >> 2, lane & 3
        lo = [f(c0[lane]), f(c0[lane] >> 16)]
        hi = [f(c1[lane]), f(c1[lane] >> 16)]
        for s in range(4):
            fr = g * 4 + s
            A = [(f(x[fr]), f(x[fr] >> 16)) for x in (a0, a1, a2, a3)]
            for j in range(2):
                col = (2 * t + j) * 4 + s
                B0 = (f(b0[col]), f(b0[col] >> 16))
                B1 = (f(b1[col]), f(b1[col] >> 16))
                lo[j] += A[0][0] * B0[0] + A[0][1] * B0[1] + A[2][0] * B1[0] + A[2][1] * B1[1]
                hi[j] += A[1][0] * B0[0] + A[1][1] * B0[1] + A[3][0] * B1[0] + A[3][1] * B1[1]
        d0.append(pack(*lo))
        d1.append(pack(*hi))
    return d0, d1


def movm(v):
    out = []
    for lane in range(32):
        r, q = lane >> 2, lane & 3
        sh = (r & 1) * 16
        lo = v[2 * q * 4 + (r >> 1)]
        hi = v[(2 * q + 1) * 4 + (r >> 1)]
        out.append(((lo >> sh) & 0xFFFF) | (((hi >> sh) & 0xFFFF) << 16))
    return out


A = rng.uniform(-1, 1, (16, 16)).astype(np.float16)
Bm = rng.uniform(-1, 1, (16, 8)).astype(np.float16)
# A row-major at byte 0 (row stride 32 bytes); B row-major [k][n] at byte 1024 (row stride 16 bytes)
for r in range(16):
    for k in range(16):
        store16(r * 32 + 2 * k, h(A[r, k]))
for k in range(16):
    for n in range(8):
        store16(1024 + k * 16 + 2 * n, h(Bm[k, n]))
# ldmatrix.x4 for A: matrix i = (rows (i&1)*8.., k (i>>1)*8..); lane l gives the address of row l%8 of matrix l//8
addrA = [((l // 8) & 1) * 8 * 32 + (l % 8) * 32 + ((l // 8) >> 1) * 16 for l in range(32)]
a = [ldsm(addrA, i, False) for i in range(4)]
# B with .trans: matrix i covers k rows i*8.. (only i=0,1 used for one n8 tile)
addrB = [1024 + (l // 8) * 8 * 16 + (l % 8) * 16 for l in range(32)]
b = [ldsm(addrB, i, True) for i in range(2)]
z = [0] * 32
d0, d1 = hmma(a[0], a[1], a[2], a[3], b[0], b[1], z, z)
D = np.zeros((16, 8), np.float32)
for lane in range(32):
    g, t = lane >> 2, lane & 3
    D[g, 2 * t], D[g, 2 * t + 1] = f(d0[lane]), f(d0[lane] >> 16)
    D[g + 8, 2 * t], D[g + 8, 2 * t + 1] = f(d1[lane]), f(d1[lane] >> 16)
ref = A.astype(np.float32) @ Bm.astype(np.float32)
print("HMMA max |D - A*B|:", float(np.abs(D - ref).max()))
M = rng.uniform(-1, 1, (8, 8)).astype(np.float16)
v = [pack(M[l >> 2, 2 * (l & 3)], M[l >> 2, 2 * (l & 3) + 1]) for l in range(32)]
T = movm(v)
MT = np.array([[f(T[r * 4 + c // 2] >> (16 * (c & 1))) for c in range(8)] for r in range(8)])
print("MOVM transpose exact:", bool((MT == M.T.astype(np.float32)).all()))
