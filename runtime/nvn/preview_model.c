// The Customize page's previews, read from the game's own files: Link in each outfit, standing as he does when idle,
// and your Loftwing gliding. Nothing has to be seen in the game first.
//
// Where they are (romfs): Object/NX/ObjectPack.arc.LZ (LZ11) holds the U8 archives oarc/Alink.arc (Link: both outfits,
// his head, face and hands as models of their own), oarc/Bird_Link.arc (the Crimson Loftwing) and oarc/PLCommonAnime.arc
// (Link's animations, each LZ11 too); the Loftwing's animations are in Skyloft's stage, Stage/F000/NX/F000_stg_l0.arc.LZ
// (oarc/BirdAnm.arc). Each model archive has g3d/model.brres - the Wii's models (MDL0: bones, vertex arrays, GX display
// lists) with textures stubbed out - and g3d/model.xtx, the Switch textures: the stubs' in order (ASTC, block-linear).
//
// Written as the menu's preview files (version 3): "LWPV", 3, vertex count, texture count; the vertices (position,
// normal, uv, texture), facing +z, y up; then each texture: its part code (a Link KIND_*, 0x100 | a BIRD_* for the bird,
// ~0 for none: never recoloured), width, height, RGBA8.
#ifndef __APPLE__
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "astc.h"
#include "tegra.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)

extern bool romfs_find_file(const char *rel, int *out_fd, uint64_t *out_off, uint64_t *out_size);
extern int link_part_kind(uint32_t vertices, bool rigid, const char *texture);  // (exec.c: what part of him a mesh is)
extern int bird_part_kind(const char *texture);                                // (exec.c: 0x100 | its part, or ~0)

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int32_t bes32(const uint8_t *p) { return (int32_t)be32(p); }
static float bef(const uint8_t *p) { uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }

typedef struct { const uint8_t *d; size_t n; } Blob;

// ---- files
static uint8_t *romfs_read(const char *path, size_t *n) {
    int fd;
    uint64_t off, sz;
    if (!romfs_find_file(path, &fd, &off, &sz)) return NULL;
    uint8_t *d = malloc(sz ? sz : 1);
    if (d && pread(fd, d, sz, (off_t)off) != (ssize_t)sz) free(d), d = NULL;
    *n = (size_t)sz;
    return d;
}

// Nintendo LZ10 / LZ11 (type byte, 24-bit size, or 0 and a 32-bit one): at most `limit` bytes of it
static uint8_t *lz_decompress(const uint8_t *in, size_t n, size_t limit, size_t *out_n) {
    if (n < 4 || (in[0] != 0x10 && in[0] != 0x11)) return NULL;
    const int type = in[0];
    size_t size = in[1] | in[2] << 8 | in[3] << 16, p = 4;
    if (!size) {
        if (n < 8) return NULL;
        size = in[4] | in[5] << 8 | in[6] << 16 | (uint32_t)in[7] << 24, p = 8;
    }
    if (limit && limit < size) size = limit;
    uint8_t *out = malloc(size ? size : 1);
    if (!out) return NULL;
    size_t o = 0;
    while (o < size && p < n) {
        uint8_t flags = in[p++];
        for (int b = 0; b < 8 && o < size && p < n; b++, flags <<= 1) {
            if (!(flags & 0x80)) { out[o++] = in[p++]; continue; }
            if (p + 4 > n) { p = n; break; }
            uint32_t len, disp;
            if (type == 0x10) len = (in[p] >> 4) + 3, disp = ((in[p] & 15) << 8 | in[p + 1]) + 1, p += 2;
            else {
                const int ind = in[p] >> 4;
                if (ind == 0) len = ((in[p] & 15) << 4 | in[p + 1] >> 4) + 0x11, disp = ((in[p + 1] & 15) << 8 | in[p + 2]) + 1, p += 3;
                else if (ind == 1)
                    len = ((in[p] & 15) << 12 | in[p + 1] << 4 | in[p + 2] >> 4) + 0x111, disp = ((in[p + 2] & 15) << 8 | in[p + 3]) + 1, p += 4;
                else len = ind + 1, disp = ((in[p] & 15) << 8 | in[p + 1]) + 1, p += 2;
            }
            if (disp > o) { free(out); return NULL; }
            for (uint32_t i = 0; i < len && o < size; i++, o++) out[o] = out[o - disp];
        }
    }
    *out_n = o;
    return out;
}

// A U8 archive's file by its path ("oarc/Alink.arc"); `end`: where its data ends, for reading just enough of one
static bool u8_find(Blob a, const char *want, Blob *out, size_t *end) {
    if (a.n < 32 || be32(a.d) != 0x55aa382d) return false;
    const uint32_t root = be32(a.d + 4);
    if ((size_t)root + 12 > a.n) return false;
    const uint32_t n = be32(a.d + root + 8);
    const size_t names = root + (size_t)n * 12;
    if (names > a.n) return false;
    struct { uint32_t end; size_t len; } stack[16];
    int depth = 0;
    char path[512] = "";
    size_t plen = 0;
    for (uint32_t i = 1; i < n; i++) {
        const uint8_t *e = a.d + root + (size_t)i * 12;
        while (depth && i >= stack[depth - 1].end) plen = stack[--depth].len, path[plen] = 0;
        const size_t no = names + (e[1] << 16 | e[2] << 8 | e[3]);
        if (no >= a.n) return false;
        const char *nm = (const char *)a.d + no;
        const uint32_t off = be32(e + 4), size = be32(e + 8);
        if (e[0] == 1) {  // (a folder: to its end)
            if (depth == 16) return false;
            stack[depth].end = size, stack[depth++].len = plen;
            if (strcmp(nm, ".") && plen + strlen(nm) + 2 < sizeof path) plen += (size_t)sprintf(path + plen, "%s/", nm);
            continue;
        }
        if (plen + strlen(nm) + 1 >= sizeof path) continue;
        strcpy(path + plen, nm);
        const bool hit = !strcmp(path, want);
        path[plen] = 0;
        if (!hit) continue;
        if (end) *end = (size_t)off + size;
        if ((size_t)off + size > a.n) return false;
        out->d = a.d + off, out->n = size;
        return true;
    }
    return false;
}

// ---- BRRES / NW4R index groups: {size, count, (count + 1) entries of 16 bytes: ..., name, data (from the group)}
static uint32_t group_count(Blob b, uint32_t g) { return (size_t)g + 8 <= b.n ? be32(b.d + g + 4) : 0; }
static const char *group_entry(Blob b, uint32_t g, uint32_t i, uint32_t *data) {
    const size_t e = (size_t)g + 8 + (size_t)(i + 1) * 16;
    if (e + 16 > b.n) return NULL;
    *data = g + be32(b.d + e + 12);
    const uint32_t no = be32(b.d + e + 8);
    return no && g + no < b.n ? (const char *)b.d + g + no : "";
}
static bool group_find(Blob b, uint32_t g, const char *name, uint32_t *data) {
    for (uint32_t i = 0; i < group_count(b, g); i++) {
        const char *nm = group_entry(b, g, i, data);
        if (nm && !strcmp(nm, name)) return true;
    }
    return false;
}
// A BRRES's folder ("3DModels(NW4R)", "Textures(NW4R)", "AnmChr(NW4R)", "External"): its index group
static bool brres_folder(Blob b, const char *name, uint32_t *g) {
    if (b.n < 16 || memcmp(b.d, "bres", 4)) return false;
    const uint32_t root = be16(b.d + 12);
    return group_find(b, root + 8, name, g);
}

// ---- 3x4 matrices (row-major, the last column the translation)
typedef struct { float m[12]; } Mat;
static Mat mat_mul(const Mat *a, const Mat *b) {
    Mat r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            r.m[i * 4 + j] = a->m[i * 4] * b->m[j] + a->m[i * 4 + 1] * b->m[4 + j] + a->m[i * 4 + 2] * b->m[8 + j] + (j == 3 ? a->m[i * 4 + 3] : 0);
    return r;
}
static Mat mat_srt(const float s[3], const float r[3], const float t[3]) {  // T * Rz * Ry * Rx * S (degrees)
    const float cx = cosf(r[0] * (float)M_PI / 180), sx = sinf(r[0] * (float)M_PI / 180), cy = cosf(r[1] * (float)M_PI / 180),
                sy = sinf(r[1] * (float)M_PI / 180), cz = cosf(r[2] * (float)M_PI / 180), sz = sinf(r[2] * (float)M_PI / 180);
    const float R[9] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, -sy, cy * sx, cy * cx};
    Mat m;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m.m[i * 4 + j] = R[i * 3 + j] * s[j];
        m.m[i * 4 + 3] = t[i];
    }
    return m;
}

// ---- MDL0 (version 11)
#define MAX_BONES 64
typedef struct {
    const char *name;
    int parent;
    uint32_t mid;  // (its draw matrix)
    float s[3], r[3], t[3];
    Mat inv;  // (model space to its own, as bound)
} Bone;
typedef struct { const uint8_t *data; uint32_t comps, fmt, div, stride, count; } Array;
typedef struct {
    Blob b;
    uint32_t o;  // (where it starts in the BRRES)
    int nbones;
    Bone bone[MAX_BONES];
    int32_t secs[14];
} Mdl;

static bool mdl_open(Blob b, uint32_t o, Mdl *m) {
    memset(m, 0, sizeof *m);
    m->b = b, m->o = o;
    if ((size_t)o + 16 + 14 * 4 > b.n || memcmp(b.d + o, "MDL0", 4) || be32(b.d + o + 8) != 11) return false;
    for (int i = 0; i < 14; i++) m->secs[i] = bes32(b.d + o + 16 + i * 4);
    if (!m->secs[1]) return false;
    const uint32_t g = o + m->secs[1];
    uint32_t offs[MAX_BONES];
    m->nbones = (int)group_count(b, g);
    if (m->nbones > MAX_BONES) return false;
    for (int i = 0; i < m->nbones; i++) {
        Bone *x = &m->bone[i];
        x->name = group_entry(b, g, (uint32_t)i, &offs[i]);
        const uint8_t *p = b.d + offs[i];
        if (!x->name || offs[i] + 0xd0 > b.n) return false;
        x->mid = be32(p + 0x10);
        for (int k = 0; k < 3; k++) x->s[k] = bef(p + 0x20 + k * 4), x->r[k] = bef(p + 0x2c + k * 4), x->t[k] = bef(p + 0x38 + k * 4);
        for (int k = 0; k < 12; k++) x->inv.m[k] = bef(p + 0xa0 + k * 4);
        x->parent = bes32(p + 0x5c) ? (int)(offs[i] + bes32(p + 0x5c)) : -1;  // (an offset, for now)
    }
    for (int i = 0; i < m->nbones; i++) {
        if (m->bone[i].parent < 0) continue;
        int par = -1;
        for (int j = 0; j < i; j++) if (offs[j] == (uint32_t)m->bone[i].parent) par = j;
        m->bone[i].parent = par;
    }
    return true;
}
static int mdl_bone(const Mdl *m, const char *name) {
    for (int i = 0; i < m->nbones; i++) if (!strcmp(m->bone[i].name, name)) return i;
    return -1;
}
static int mdl_bone_of(const Mdl *m, uint32_t mid) {
    for (int i = 0; i < m->nbones; i++) if (m->bone[i].mid == mid) return i;
    return -1;
}
// Its bones in the world: each its own (or an animation's) scale, rotation and translation under its parent's
typedef struct { const char *bone; float v[9]; uint8_t has; } Key;  // (has: 1 scale, 2 rotation, 4 translation)
static void mdl_pose(const Mdl *m, const Key *keys, int nkeys, Mat *world) {
    for (int i = 0; i < m->nbones; i++) {
        const Bone *x = &m->bone[i];
        float s[3], r[3], t[3];
        memcpy(s, x->s, 12), memcpy(r, x->r, 12), memcpy(t, x->t, 12);
        for (int k = 0; k < nkeys; k++) {
            if (strcmp(keys[k].bone, x->name)) continue;
            if (keys[k].has & 1) memcpy(s, keys[k].v, 12);
            if (keys[k].has & 2) memcpy(r, keys[k].v + 3, 12);
            if (keys[k].has & 4) memcpy(t, keys[k].v + 6, 12);
        }
        const Mat l = mat_srt(s, r, t);
        world[i] = x->parent >= 0 ? mat_mul(&world[x->parent], &l) : l;
    }
}
static bool mdl_array(const Mdl *m, int sec, int idx, Array *a) {
    if (idx < 0 || !m->secs[sec]) return false;
    const uint32_t g = m->o + m->secs[sec];
    for (uint32_t i = 0; i < group_count(m->b, g); i++) {
        uint32_t ao;
        if (!group_entry(m->b, g, i, &ao) || ao + 0x20 > m->b.n || be32(m->b.d + ao + 0x10) != (uint32_t)idx) continue;
        const uint8_t *p = m->b.d + ao;
        const uint32_t ct = be32(p + 0x14);
        a->fmt = be32(p + 0x18), a->div = p[0x1c], a->stride = p[0x1d], a->count = be16(p + 0x1e);
        a->comps = sec == 2 ? 2 + ct : sec == 5 ? 1 + ct : 3;
        a->data = p + bes32(p + 8);
        if (a->data + (size_t)a->count * a->stride > m->b.d + m->b.n || a->fmt > 4) return false;
        if (sec == 3) a->div = a->fmt == 1 ? 6 : a->fmt == 3 ? 14 : 0;  // (normals: fixed fractions)
        return true;
    }
    return false;
}
static void array_get(const Array *a, uint32_t i, float *out) {
    for (uint32_t k = 0; k < 3; k++) out[k] = 0;
    if (i >= a->count) return;
    const uint8_t *p = a->data + (size_t)i * a->stride;
    for (uint32_t k = 0; k < a->comps && k < 3; k++) {
        float v;
        switch (a->fmt) {
        case 0: v = p[k]; break;
        case 1: v = (int8_t)p[k]; break;
        case 2: v = (float)be16(p + k * 2); break;
        case 3: v = (int16_t)be16(p + k * 2); break;
        default: v = bef(p + k * 4); break;
        }
        out[k] = a->fmt == 4 ? v : v / (float)(1 << a->div);
    }
}
// Its draw matrices: a bone's, or (NodeMix) a weighted mix of bones'
#define MAX_MIX 8
typedef struct { int n; int bone[MAX_MIX]; float w[MAX_MIX]; } Mix;
static bool mdl_mix(const Mdl *m, uint32_t mid, Mix *mix) {
    uint32_t p;
    if (m->secs[0] && group_find(m->b, m->o + m->secs[0], "NodeMix", &p)) {
        while (p < m->b.n && m->b.d[p] != 1) {
            const uint8_t *q = m->b.d + p;
            if (q[0] == 3) {
                const uint32_t n = q[3];
                if (be16(q + 1) == mid) {
                    mix->n = 0;
                    for (uint32_t k = 0; k < n && k < MAX_MIX; k++) {
                        const int bi = mdl_bone_of(m, be16(q + 4 + k * 6));
                        if (bi < 0) return false;
                        mix->bone[mix->n] = bi, mix->w[mix->n++] = bef(q + 6 + k * 6);
                    }
                    return mix->n > 0;
                }
                p += 4 + n * 6;
            } else if (q[0] == 5) p += 5;
            else break;
        }
    }
    const int bi = mdl_bone_of(m, mid);
    if (bi < 0) return false;
    mix->n = -1, mix->bone[0] = bi;  // (one bone: its vertices are in its own space)
    return true;
}

// Its meshes: each object's material, and that material's first texture
typedef struct { int mat; const char *texture; } ObjMat;
static bool mdl_objmat(const Mdl *m, uint32_t obj, ObjMat *om) {
    static const char *const LISTS[] = {"DrawOpa", "DrawXlu"};
    om->mat = -1, om->texture = NULL;
    for (int l = 0; l < 2 && om->mat < 0; l++) {
        uint32_t p;
        if (!m->secs[0] || !group_find(m->b, m->o + m->secs[0], LISTS[l], &p)) continue;
        for (; p + 8 <= m->b.n && m->b.d[p] == 4; p += 8)
            if (be16(m->b.d + p + 3) == obj) { om->mat = (int)be16(m->b.d + p + 1); break; }
    }
    if (om->mat < 0 || !m->secs[8] || !m->secs[11]) return false;
    uint32_t mo;
    if (!group_entry(m->b, m->o + m->secs[8], (uint32_t)om->mat, &mo)) return false;
    const uint32_t g = m->o + m->secs[11];
    uint32_t best = ~0u;
    for (uint32_t i = 0; i < group_count(m->b, g); i++) {
        uint32_t to;
        const char *nm = group_entry(m->b, g, i, &to);
        if (!nm || to + 4 > m->b.n) continue;
        for (uint32_t k = 0, n = be32(m->b.d + to); k < n && to + 12 + k * 8 <= m->b.n; k++) {
            const uint32_t at = to + 4 + k * 8;  // (a material and its layer using this texture: from the entry's start)
            if ((int64_t)to + bes32(m->b.d + at) != (int64_t)mo) continue;
            const uint32_t layer = to + bes32(m->b.d + at + 4);
            if (layer < best) best = layer, om->texture = nm;  // (its first layer)
        }
    }
    return om->texture != NULL;
}

// ---- the output
typedef struct { float p[3], n[3], uv[2]; uint32_t tex; } Vtx;
typedef struct { const char *name; uint32_t code; } TexUse;
typedef struct {
    Vtx *v;
    uint32_t n, cap;
    TexUse use[32];
    uint32_t nuse;
} Out;
static int out_tex(Out *o, const char *name, uint32_t code) {
    for (uint32_t i = 0; i < o->nuse; i++) if (o->use[i].code == code && !strcmp(o->use[i].name, name)) return (int)i;
    if (o->nuse == 32) return -1;
    o->use[o->nuse].name = name, o->use[o->nuse].code = code;
    return (int)o->nuse++;
}
static Vtx *out_add(Out *o, uint32_t n) {
    if (o->n + n > o->cap) {
        uint32_t cap = (o->n + n) * 2;
        Vtx *v = realloc(o->v, sizeof *v * cap);
        if (!v) return NULL;
        o->v = v, o->cap = cap;
    }
    o->n += n;
    return o->v + o->n - n;
}

// One object's triangles, posed (world: each bone's matrix), into `out` with texture `tex`
typedef struct { const Mdl *m; const Mat *world; int tex; } Ctx;
static bool skin_vertex(const Ctx *c, uint32_t mid, const Array *pos, const Array *nrm, const Array *uv, const uint32_t idx[3], Vtx *o) {
    Mix mix;
    if (!mdl_mix(c->m, mid, &mix)) return false;
    Mat X;
    if (mix.n < 0) X = c->world[mix.bone[0]];
    else {
        memset(&X, 0, sizeof X);
        for (int k = 0; k < mix.n; k++) {
            const Mat s = mat_mul(&c->world[mix.bone[k]], &c->m->bone[mix.bone[k]].inv);
            for (int j = 0; j < 12; j++) X.m[j] += mix.w[k] * s.m[j];
        }
    }
    float p[3], n[3] = {0, 0, 0}, t[3] = {0, 0, 0};
    array_get(pos, idx[0], p);
    if (nrm->data) array_get(nrm, idx[1], n);
    if (uv->data) array_get(uv, idx[2], t);
    float len = 0;
    for (int i = 0; i < 3; i++) {
        o->p[i] = X.m[i * 4] * p[0] + X.m[i * 4 + 1] * p[1] + X.m[i * 4 + 2] * p[2] + X.m[i * 4 + 3];
        o->n[i] = X.m[i * 4] * n[0] + X.m[i * 4 + 1] * n[1] + X.m[i * 4 + 2] * n[2];
        len += o->n[i] * o->n[i];
    }
    if (len > 0) for (int i = 0; i < 3; i++) o->n[i] /= sqrtf(len);
    o->uv[0] = t[0], o->uv[1] = t[1], o->tex = (uint32_t)c->tex;
    return true;
}
// Its GX display list: its triangles (lists, strips, fans, quads), the matrices it loads (0x20) for weighted vertices.
// Returns its triangle count, or -1.
static long mdl_object(const Ctx *c, uint32_t obj, Out *out, bool count_only) {
    const Mdl *m = c->m;
    uint32_t po;
    if (!m->secs[10] || !group_entry(m->b, m->o + m->secs[10], obj, &po) || po + 0x68 > m->b.n) return -1;
    const uint8_t *P = m->b.d + po;
    const int32_t node = bes32(P + 8);
    const uint32_t vlo = be32(P + 0xc), vhi = be32(P + 0x10), dsize = be32(P + 0x24);
    const size_t dl = po + 0x24 + bes32(P + 0x2c);
    if (dl + dsize > m->b.n) return -1;
    Array pos = {0}, nrm = {0}, uv = {0};
    if (!mdl_array(m, 2, (int16_t)be16(P + 0x48), &pos)) return -1;
    mdl_array(m, 3, (int16_t)be16(P + 0x4a), &nrm);
    mdl_array(m, 5, (int16_t)be16(P + 0x50), &uv);
    // (its vertices' attributes, in order: matrix index, 8 texture matrix indices, position, normal, 2 colours, 8 uvs;
    //  each none, direct, 8-bit index or 16-bit)
    int type[21];
    type[0] = vlo & 1;
    for (int k = 0; k < 8; k++) type[1 + k] = vlo >> (1 + k) & 1;
    for (int k = 0; k < 4; k++) type[9 + k] = vlo >> (9 + 2 * k) & 3;
    for (int k = 0; k < 8; k++) type[13 + k] = vhi >> (2 * k) & 3;
    uint32_t slot[10] = {0};
    long tris = 0;
    const uint8_t *p = m->b.d + dl, *end = p + dsize;
    while (p < end) {
        const uint8_t cmd = *p++;
        if (cmd == 0) continue;
        if (cmd == 0x08) { p += 5; continue; }
        if (cmd == 0x10) { p += 4 + (be16(p) + 1) * 4; continue; }
        if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
            if (cmd == 0x20 && (be16(p + 2) & 0xfff) / 12 < 10) slot[(be16(p + 2) & 0xfff) / 12] = be16(p);
            p += 4;
            continue;
        }
        const uint8_t op = cmd & 0xf8;
        if (op != 0x80 && op != 0x90 && op != 0x98 && op != 0xa0) return -1;
        const uint32_t n = be16(p);
        p += 2;
        Vtx q[4], a = {0}, b = {0};
        for (uint32_t i = 0; i < n; i++) {
            uint32_t val[21] = {0};
            for (int k = 0; k < 21; k++) {
                if (!type[k]) continue;
                if (p + (type[k] == 3 ? 2 : 1) > end) return -1;
                if (type[k] == 3) val[k] = be16(p), p += 2;
                else val[k] = *p++;
            }
            if (count_only) continue;
            const uint32_t mid = type[0] ? slot[val[0] / 3 < 10 ? val[0] / 3 : 0] : (uint32_t)node;
            const uint32_t idx[3] = {val[9], val[10], val[13]};
            Vtx v;
            if (!skin_vertex(c, mid, &pos, &nrm, &uv, idx, &v)) return -1;
            // (each as triangles: a list's in threes, a quad's in fours; a strip's from its last two, every other one turned
            //  round; a fan's from its first and last)
            Vtx *t;
            if (op == 0x90 || op == 0x80) {
                const uint32_t per = op == 0x90 ? 3 : 4;
                q[i % per] = v;
                if (i % per == per - 1 && (t = out_add(out, per == 3 ? 3 : 6))) {
                    t[0] = q[0], t[1] = q[1], t[2] = q[2];
                    if (per == 4) t[3] = q[0], t[4] = q[2], t[5] = q[3];
                }
            } else if (i >= 2 && (t = out_add(out, 3))) {
                if (op == 0xa0 || i % 2 == 0) t[0] = a, t[1] = b, t[2] = v;
                else t[0] = b, t[1] = a, t[2] = v;
            }
            if (op == 0x98) a = i ? b : v, b = v;
            else if (op == 0xa0) a = i ? a : v, b = v;
        }
        tris += op == 0x90 ? n / 3 : op == 0x80 ? n / 2 : n >= 2 ? n - 2 : 0;
    }
    return tris;
}

// ---- CHR0 (version 5): each bone's scale, rotation and translation at its first frame
static float chr0_key(const uint8_t *e, int fmt, int32_t off, const uint8_t *end) {
    const uint8_t *q = e + off;
    if (q + 24 > end) return 0;
    switch (fmt) {
    case 1: return bef(q + 12) + bef(q + 8) * (float)(be32(q + 16) >> 12 & 0xfff);  // (interpolated, 4-byte keys)
    case 2: return bef(q + 12) + bef(q + 8) * (float)be16(q + 18);                  // (6-byte keys)
    case 3: return bef(q + 12);                                                     // (12-byte keys: frame, value, slope)
    case 4: return bef(q + 4) + bef(q) * (float)q[8];                               // (one per frame, 8 bits)
    case 5: return bef(q + 4) + bef(q) * (float)be16(q + 8);                        // (16 bits)
    case 6: return bef(q);                                                          // (floats)
    default: return 0;
    }
}
static int chr0_first(Blob b, uint32_t o, Key *keys, int max) {
    if ((size_t)o + 0x28 > b.n || memcmp(b.d + o, "CHR0", 4) || be32(b.d + o + 8) != 5) return 0;
    const uint32_t g = o + bes32(b.d + o + 0x10);
    int nk = 0;
    for (uint32_t i = 0; i < group_count(b, g) && nk < max; i++) {
        uint32_t e;
        const char *nm = group_entry(b, g, i, &e);
        if (!nm || e + 8 > b.n) continue;
        const uint32_t code = be32(b.d + e + 4);
        const uint8_t *p = b.d + e + 8;
        Key *k = &keys[nk++];
        memset(k, 0, sizeof *k);
        k->bone = nm;
        static const struct { int has, iso, fixed, fmt_shift, fmt_mask; } PARTS[3] = {{22, 4, 13, 25, 3}, {23, 5, 16, 27, 7}, {24, 6, 19, 30, 3}};
        for (int part = 0; part < 3; part++) {
            if (!(code >> PARTS[part].has & 1)) continue;
            const int fmt = code >> PARTS[part].fmt_shift & PARTS[part].fmt_mask, iso = code >> PARTS[part].iso & 1;
            for (int c = 0; c < (iso ? 1 : 3); c++, p += 4) {
                if (p + 4 > b.d + b.n) return nk;
                const bool fixed = code >> (PARTS[part].fixed + (iso ? 0 : c)) & 1;
                const float v = fixed ? bef(p) : chr0_key(b.d + e, fmt, bes32(p), b.d + b.n);
                for (int r = 0; r < (iso ? 3 : 1); r++) k->v[part * 3 + (iso ? r : c)] = v;
            }
            k->has |= (uint8_t)(1 << part);
        }
    }
    return nk;
}

// ---- textures: model.xtx's, the BRRES's stubs (1x1 TEX0s) in order
typedef struct { uint8_t *rgba; uint32_t w, h; } Image;
static bool xtx_texture(Blob brres, Blob xtx, const char *name, Image *img) {
    uint32_t g, rank = 0, i;
    if (!brres_folder(brres, "Textures(NW4R)", &g)) return false;
    for (i = 0; i < group_count(brres, g); i++) {
        uint32_t to;
        const char *nm = group_entry(brres, g, i, &to);
        if (!nm || to + 0x20 > brres.n) return false;
        if (!strcmp(nm, name)) break;
        if (be16(brres.d + to + 0x1c) == 1 && be16(brres.d + to + 0x1e) == 1) rank++;
    }
    if (i == group_count(brres, g) || xtx.n < 16 || memcmp(xtx.d, "DFvN", 4)) return false;
    uint32_t w = 0, h = 0, fmt = 0, levels = 0, seen = 0;
    for (size_t o = xtx.d[4] | xtx.d[5] << 8; o + 0x24 <= xtx.n && !memcmp(xtx.d + o, "HBvN", 4);) {
        uint64_t dsz, doff;
        uint32_t type;
        memcpy(&dsz, xtx.d + o + 8, 8), memcpy(&doff, xtx.d + o + 16, 8), memcpy(&type, xtx.d + o + 24, 4);
        if (o + doff + dsz > xtx.n) return false;
        const uint8_t *body = xtx.d + o + doff;
        if (type == 2 && dsz >= 0x24) memcpy(&w, body + 12, 4), memcpy(&h, body + 16, 4), memcpy(&fmt, body + 28, 4), memcpy(&levels, body + 32, 4);
        if (type == 3 && seen++ == rank) {
            const FormatInfo *fi = nvn_format_info(fmt);
            if (!fi || fi->bpb != 16 || fi->bw < 2 || !w || !h || w > 4096 || h > 4096) return false;  // (ASTC only)
            TexLayout L = {w, h, 1, 1, levels ? levels : 1, fi, 0, 0};
            tegra_layout_init(&L);
            const LevelLayout l = tegra_level(&L, 0);
            if (l.offset + (uint64_t)l.wb * l.hb * 16 > dsz) return false;
            uint8_t *blocks = malloc((size_t)l.wb * l.hb * 16);
            img->rgba = malloc((size_t)w * h * 4);
            if (!blocks || !img->rgba) { free(blocks), free(img->rgba); return false; }
            tegra_deswizzle(body + l.offset, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
            astc_decode_ldr(blocks, w, h, fi->bw, fi->bh, false, img->rgba);
            free(blocks);
            img->w = w, img->h = h;
            return true;
        }
        o += doff + dsz;
    }
    return false;
}

static bool out_write(const char *file, const Out *o, Blob brres, Blob xtx) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.part", file);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    const uint32_t hdr[4] = {0x5650574c, 3, o->n, o->nuse};
    bool ok = fwrite(hdr, 4, 4, f) == 4 && fwrite(o->v, sizeof *o->v, o->n, f) == o->n;
    for (uint32_t t = 0; ok && t < o->nuse; t++) {
        Image img = {0};
        if (!xtx_texture(brres, xtx, o->use[t].name, &img)) {
            LOG("[preview] %s: texture %s not read\n", file, o->use[t].name);
            ok = false;
            break;
        }
        const uint32_t th[3] = {o->use[t].code, img.w, img.h};
        ok = fwrite(th, 4, 3, f) == 3 && fwrite(img.rgba, 4, (size_t)img.w * img.h, f) == (size_t)img.w * img.h;
        free(img.rgba);
    }
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, file) == 0;
    if (!ok) remove(tmp);
    return ok;
}

// ---- Link: his body, head, face and hands (open: their first of seven), standing as he does when idle (waitB's first
// frame); the classic tunic's cap, swung by the game as he moves, hung down his back
static bool build_link(Blob alink, Blob idle, bool classic, const char *file) {
    Blob brres, xtx;
    uint32_t models, mo;
    if (!u8_find(alink, "g3d/model.brres", &brres, NULL) || !u8_find(alink, "g3d/model.xtx", &xtx, NULL) || !brres_folder(brres, "3DModels(NW4R)", &models))
        return false;
    const char *names[4] = {classic ? "al" : "pl_body", classic ? "al_head" : "pl_head", "al_face", classic ? "al_hands" : "pl_hands"};
    static Mdl part[4];
    for (int i = 0; i < 4; i++)
        if (!group_find(brres, models, names[i], &mo) || !mdl_open(brres, mo, &part[i])) {
            LOG("[preview] Link's model %s not read\n", names[i]);
            return false;
        }
    static Key keys[MAX_BONES];
    uint32_t ag, ao;
    int nk = 0;
    if (brres_folder(idle, "AnmChr(NW4R)", &ag) && group_entry(idle, ag, 0, &ao)) nk = chr0_first(idle, ao, keys, MAX_BONES);
    static Mat body[MAX_BONES], world[MAX_BONES];
    mdl_pose(&part[0], keys, nk, body);
    const int head = mdl_bone(&part[0], "head"), handL = mdl_bone(&part[0], "handL"), handR = mdl_bone(&part[0], "handR");
    if (head < 0 || handL < 0 || handR < 0) return false;
    static const Key CAP[4] = {{"z_cap1", {0, 0, 0, 0, 0, 180 - 55}, 2}, {"z_cap2", {0, 0, 0, 0, 0, -30}, 2},
                               {"z_cap3", {0, 0, 0, 0, 0, -20}, 2}, {"z_cap4", {0, 0, 0, 0, 0, -10}, 2}};
    Out out = {0};
    for (int i = 0; i < 4; i++) {
        const Mdl *m = &part[i];
        if (i == 0) memcpy(world, body, sizeof(Mat) * m->nbones);
        else {
            mdl_pose(m, i == 1 ? CAP : NULL, i == 1 ? 4 : 0, world);
            for (int b = 0; b < m->nbones; b++) {
                const size_t len = strlen(m->bone[b].name);
                const char last = len ? m->bone[b].name[len - 1] : 0;
                if (i == 3) world[b] = body[last == 'L' ? handL : handR];  // (each hand's bone: his)
                else world[b] = mat_mul(&body[head], &world[b]);          // (his head's, and face's: on his head)
            }
        }
        for (uint32_t ob = 0; ob < group_count(brres, m->o + m->secs[10]); ob++) {
            ObjMat om;
            if (!mdl_objmat(m, ob, &om)) continue;
            uint32_t mo2;
            const char *mat = group_entry(brres, m->o + m->secs[8], (uint32_t)om.mat, &mo2);
            const size_t ml = mat ? strlen(mat) : 0;
            if (i == 3 && (ml < 3 || mat[ml - 3] != 'A')) continue;  // (each hand's first shape: open)
            const Ctx c0 = {m, world, 0};
            const long tris = mdl_object(&c0, ob, &out, true);
            if (tris <= 0) continue;
            uint32_t po;
            group_entry(brres, m->o + m->secs[10], ob, &po);
            const bool rigid = bes32(brres.d + po + 8) >= 0;
            const int kind = link_part_kind((uint32_t)tris * 3, rigid, om.texture);
            const Ctx c = {m, world, out_tex(&out, om.texture, (uint32_t)kind)};
            if (c.tex < 0 || mdl_object(&c, ob, &out, false) < 0) {
                LOG("[preview] Link's %s, mesh %u: not read\n", names[i], ob);
                free(out.v);
                return false;
            }
        }
    }
    const bool ok = out_write(file, &out, brres, xtx);
    LOG("[preview] Link (%s) from the game's files: %u vertices, %u textures%s\n", classic ? "classic tunic" : "Skyloft clothes", out.n,
        out.nuse, ok ? "" : " - not written");
    free(out.v);
    return ok;
}

// ---- the Loftwing, gliding (its Glide's first frame)
static bool build_bird(Blob arc, Blob anims, const char *file) {
    Blob brres, xtx;
    uint32_t models, mo;
    static Mdl m;
    if (!u8_find(arc, "g3d/model.brres", &brres, NULL) || !u8_find(arc, "g3d/model.xtx", &xtx, NULL) || !brres_folder(brres, "3DModels(NW4R)", &models) ||
        !group_find(brres, models, "BirdLink", &mo) || !mdl_open(brres, mo, &m))
        return false;
    static Key keys[MAX_BONES];
    uint32_t ag, ao;
    int nk = 0;
    if (brres_folder(anims, "AnmChr(NW4R)", &ag) && group_find(anims, ag, "Glide", &ao)) nk = chr0_first(anims, ao, keys, MAX_BONES);
    static Mat world[MAX_BONES];
    mdl_pose(&m, keys, nk, world);
    Out out = {0};
    for (uint32_t ob = 0; ob < group_count(brres, m.o + m.secs[10]); ob++) {
        ObjMat om;
        if (!mdl_objmat(&m, ob, &om)) continue;
        const Ctx c = {&m, world, out_tex(&out, om.texture, (uint32_t)bird_part_kind(om.texture))};
        if (c.tex < 0 || mdl_object(&c, ob, &out, false) < 0) { free(out.v); return false; }
    }
    const bool ok = out_write(file, &out, brres, xtx);
    LOG("[preview] Loftwing from the game's files: %u vertices, %u textures%s\n", out.n, out.nuse, ok ? "" : " - not written");
    free(out.v);
    return ok;
}

// Reads the game's files and writes the previews' files (the menu's thread for it). True if all were.
bool preview_models_build(const char *bird_file, const char *skyloft_file, const char *classic_file) {
    bool ok = false;
    size_t n, pack_n = 0, need = 0, stage_n = 0;
    uint8_t *lz = romfs_read("Object/NX/ObjectPack.arc.LZ", &n), *pack = NULL, *stage = NULL, *idle = NULL;
    if (!lz) { LOG("[preview] Object/NX/ObjectPack.arc.LZ not found\n"); return false; }
    // (only as much of it as holds what's wanted: its table first - the start of it - then up to the last of them)
    static const char *const WANT[] = {"oarc/Alink.arc", "oarc/Bird_Link.arc", "oarc/PLCommonAnime.arc"};
    if ((pack = lz_decompress(lz, n, 1 << 18, &pack_n))) {
        for (int i = 0; i < 3; i++) {
            Blob x;
            size_t e = 0;
            u8_find((Blob){pack, pack_n}, WANT[i], &x, &e);  // (its end, though its data isn't here yet)
            if (e > need) need = e;
        }
        free(pack);
        pack = need ? lz_decompress(lz, n, need, &pack_n) : NULL;
    }
    free(lz);
    if (!pack || pack_n < need) { LOG("[preview] ObjectPack not read\n"); free(pack); return false; }
    const Blob P = {pack, pack_n};
    Blob alink, birdarc, anime, animbrres, wait;
    uint32_t eg, eo;
    if (u8_find(P, "oarc/Alink.arc", &alink, NULL) && u8_find(P, "oarc/PLCommonAnime.arc", &anime, NULL) &&
        u8_find(anime, "g3d/model.brres", &animbrres, NULL) && brres_folder(animbrres, "External", &eg) && group_find(animbrres, eg, "waitB.rchaC", &eo)) {
        size_t in = 0;
        idle = lz_decompress(animbrres.d + eo, animbrres.n - eo, 0, &in);
        wait = (Blob){idle, idle ? in : 0};
        ok = idle && build_link(alink, wait, false, skyloft_file);
        ok = idle && build_link(alink, wait, true, classic_file) && ok;
    } else LOG("[preview] Link's files not found\n");
    // (the Loftwing's animations: in Skyloft's stage)
    uint8_t *slz = romfs_read("Stage/F000/NX/F000_stg_l0.arc.LZ", &n);
    Blob anims = {NULL, 0}, ba, bb;
    if (slz) {
        size_t sn = 0, e = 0;
        uint8_t *shead = lz_decompress(slz, n, 1 << 18, &sn);
        if (shead) u8_find((Blob){shead, sn}, "oarc/BirdAnm.arc", &ba, &e);
        free(shead);
        if (e && (stage = lz_decompress(slz, n, e, &stage_n)) && u8_find((Blob){stage, stage_n}, "oarc/BirdAnm.arc", &ba, NULL) &&
            u8_find(ba, "g3d/model.brres", &bb, NULL))
            anims = bb;
        free(slz);
    }
    if (u8_find(P, "oarc/Bird_Link.arc", &birdarc, NULL)) ok = build_bird(birdarc, anims, bird_file) && ok;
    else ok = false, LOG("[preview] the Loftwing's files not found\n");
    free(stage), free(idle), free(pack);
    return ok;
}
#endif
