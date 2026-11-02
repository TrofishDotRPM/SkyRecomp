// See coop_pose.h.
#include "coop_pose.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BONES 1024
#define MAX_PARTS 64
#define MAX_SLOTS 4096
#define ROT_SCALE 16000.0f  // bone rotations are near unit size
#define POS_SCALE 32.0f     // 1/32 of a game unit; +-1000 units from his feet
#define HASH_SLOTS 4096

// Named by its vertex count, kind and where in memory its mesh is (tag): Link has pieces of the same size, both hands.
// hidden: drawn with all its faces culled (the game's way of hiding his blade in its sheath).
typedef struct { uint32_t vertices; uint16_t count, tag; uint8_t rigid, hidden; uint32_t first; } Part;  // first: into slots

typedef struct {
    float feet[3];
    uint16_t nbones, nparts;
    uint32_t nslots;
    uint32_t time_ms;  // sender's clock
    Part parts[MAX_PARTS];
    uint16_t slots[MAX_SLOTS];
    float bones[MAX_BONES][12];  // feet-relative: rotation rows, translation in the last column
} Pose;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Pose g_gather, g_ready;       // sending side: g_gather only on the render thread
static uint64_t g_gather_frame = ~0ull;
static bool g_ready_new;
// Receiving side: the last few, oldest first, played a little behind so there's nearly always a next one to blend towards
#define RING 8
static Pose g_ring[RING];
static uint32_t g_ring_time[RING];  // sender's clock
static int g_ring_n;
static int64_t g_offset;            // our clock - sender's, as small as seen: the quickest a pose has ever got here
static bool g_have_offset;
static uint64_t g_used_frame = ~0ull;  // parts handed out this frame: Link has several with the same name
static uint8_t g_used[MAX_PARTS];

static int16_t quant(float v, float scale) {
    float q = roundf(v * scale);
    return q > 32767 ? 32767 : q < -32768 ? -32768 : (int16_t)q;
}

static uint32_t bone_hash(const float *m) {
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)m;
    for (int i = 0; i < 48; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// View space to relative to his feet. The view's rotation is orthonormal: its transpose undoes it.
static void to_relative(float out[12], const float m[12], const float v[12], const float feet[3]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) out[r * 4 + c] = v[0 + r] * m[c] + v[4 + r] * m[4 + c] + v[8 + r] * m[8 + c];
        float tx = m[3] - v[3], ty = m[7] - v[7], tz = m[11] - v[11];
        out[r * 4 + 3] = v[0 + r] * tx + v[4 + r] * ty + v[8 + r] * tz - feet[r];
    }
}

static void from_relative(float out[12], float *rot, const float m[12], const float v[12], const float feet[3]) {
    float t[3] = {m[3] + feet[0], m[7] + feet[1], m[11] + feet[2]};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out[r * 4 + c] = v[r * 4] * m[c] + v[r * 4 + 1] * m[4 + c] + v[r * 4 + 2] * m[8 + c];
            if (rot) rot[r * 4 + c] = out[r * 4 + c];
        }
        out[r * 4 + 3] = v[r * 4] * t[0] + v[r * 4 + 1] * t[1] + v[r * 4 + 2] * t[2] + v[r * 4 + 3];
        if (rot) rot[r * 4 + 3] = 0;
    }
}

void coop_pose_capture(uint64_t frame, uint64_t now_ms, uint32_t vertices, uint16_t tag, bool rigid, bool hidden, const float *bones, uint32_t count,
                       const float view[12], const float feet[3]) {
    static int32_t table[HASH_SLOTS];  // bone index + 1, this frame
    if (frame != g_gather_frame) {
        if (g_gather_frame != ~0ull && g_gather.nparts) {
            pthread_mutex_lock(&g_lock);
            g_ready = g_gather;
            g_ready_new = true;
            pthread_mutex_unlock(&g_lock);
        }
        g_gather_frame = frame;
        g_gather.nbones = g_gather.nparts = 0, g_gather.nslots = 0;
        g_gather.time_ms = (uint32_t)now_ms;
        memcpy(g_gather.feet, feet, sizeof g_gather.feet);
        memset(table, 0, sizeof table);
    }
    Pose *p = &g_gather;
    if (p->nparts == MAX_PARTS || p->nslots + count > MAX_SLOTS || count > 0xffff) return;
    Part *part = &p->parts[p->nparts++];
    part->vertices = vertices, part->tag = tag, part->rigid = rigid, part->hidden = hidden, part->count = (uint16_t)count, part->first = p->nslots;
    for (uint32_t i = 0; i < count; i++) {
        float rel[12];
        to_relative(rel, bones + i * 12, view, p->feet);
        // The same bone recurs across a model's meshes, exactly: shared, it costs one matrix
        uint32_t h = bone_hash(bones + i * 12) & (HASH_SLOTS - 1);
        int found = -1;
        for (int probe = 0; probe < HASH_SLOTS; probe++, h = (h + 1) & (HASH_SLOTS - 1)) {
            int32_t e = table[h];
            if (!e) break;
            if (!memcmp(p->bones[e - 1], rel, sizeof rel)) { found = e - 1; break; }
        }
        if (found < 0) {
            if (p->nbones == MAX_BONES) { p->nparts--; p->nslots = part->first; return; }
            found = p->nbones++;
            memcpy(p->bones[found], rel, sizeof rel);
            table[h] = found + 1;
        }
        p->slots[p->nslots++] = (uint16_t)found;
    }
}

// ---- on the wire, little-endian: time u32, feet (3 f32), nbones u16, nparts u16, parts (vertices u32, count u16, tag u16, flags u8: 1
// rigid, 2 hidden), slots (u16 each), bones (12 i16 each)
static void put_u16(uint8_t **w, uint16_t v) { (*w)[0] = (uint8_t)v, (*w)[1] = (uint8_t)(v >> 8), *w += 2; }
static uint16_t get_u16(const uint8_t **r) { uint16_t v = (uint16_t)((*r)[0] | (*r)[1] << 8); *r += 2; return v; }

size_t coop_pose_take(uint8_t *out, size_t cap) {
    pthread_mutex_lock(&g_lock);
    if (!g_ready_new) { pthread_mutex_unlock(&g_lock); return 0; }
    const Pose *p = &g_ready;
    size_t need = 20 + (size_t)p->nparts * 9 + (size_t)p->nslots * 2 + (size_t)p->nbones * 24;
    if (need > cap) { g_ready_new = false; pthread_mutex_unlock(&g_lock); return 0; }
    uint8_t *w = out;
    put_u16(&w, (uint16_t)p->time_ms), put_u16(&w, (uint16_t)(p->time_ms >> 16));
    memcpy(w, p->feet, 12), w += 12;
    put_u16(&w, p->nbones), put_u16(&w, p->nparts);
    for (int i = 0; i < p->nparts; i++) {
        const Part *q = &p->parts[i];
        put_u16(&w, (uint16_t)q->vertices), put_u16(&w, (uint16_t)(q->vertices >> 16));
        put_u16(&w, q->count);
        put_u16(&w, q->tag);
        *w++ = (uint8_t)(q->rigid | q->hidden << 1);
    }
    for (uint32_t i = 0; i < p->nslots; i++) put_u16(&w, p->slots[i]);
    for (int b = 0; b < p->nbones; b++)
        for (int k = 0; k < 12; k++) put_u16(&w, (uint16_t)quant(p->bones[b][k], k % 4 == 3 ? POS_SCALE : ROT_SCALE));
    g_ready_new = false;
    pthread_mutex_unlock(&g_lock);
    return (size_t)(w - out);
}

bool coop_pose_receive(const uint8_t *data, size_t size, uint64_t now_ms) {
    static Pose in;
    if (size < 20) return false;
    const uint8_t *r = data, *end = data + size;
    uint32_t time_lo = get_u16(&r), time_hi = get_u16(&r);
    in.time_ms = time_lo | time_hi << 16;
    memcpy(in.feet, r, 12), r += 12;
    if (!isfinite(in.feet[0]) || !isfinite(in.feet[1]) || !isfinite(in.feet[2])) return false;
    in.nbones = get_u16(&r), in.nparts = get_u16(&r);
    if (in.nbones > MAX_BONES || in.nparts > MAX_PARTS || (size_t)(end - r) < (size_t)in.nparts * 9) return false;
    in.nslots = 0;
    for (int i = 0; i < in.nparts; i++) {
        Part *q = &in.parts[i];
        uint32_t lo = get_u16(&r), hi = get_u16(&r);
        q->vertices = lo | hi << 16;
        q->count = get_u16(&r);
        q->tag = get_u16(&r);
        q->rigid = *r & 1, q->hidden = *r >> 1 & 1, r++;
        q->first = in.nslots;
        if (in.nslots + q->count > MAX_SLOTS) return false;
        in.nslots += q->count;
    }
    if ((size_t)(end - r) != (size_t)in.nslots * 2 + (size_t)in.nbones * 24) return false;
    for (uint32_t i = 0; i < in.nslots; i++) {
        in.slots[i] = get_u16(&r);
        if (in.slots[i] >= in.nbones) return false;
    }
    for (int b = 0; b < in.nbones; b++)
        for (int k = 0; k < 12; k++) in.bones[b][k] = (int16_t)get_u16(&r) / (k % 4 == 3 ? POS_SCALE : ROT_SCALE);
    pthread_mutex_lock(&g_lock);
    int64_t off = (int64_t)now_ms - (int64_t)in.time_ms;
    if (!g_have_offset || off < g_offset) g_offset = off;
    else if (off > g_offset) g_offset++;  // (the clocks drift, and a lucky early arrival shouldn't last forever)
    g_have_offset = true;
    if (g_ring_n && (int32_t)(in.time_ms - g_ring_time[g_ring_n - 1]) <= 0) {  // (a repeat, or the other player restarted)
        if ((int32_t)(in.time_ms - g_ring_time[g_ring_n - 1]) < -1000) g_ring_n = 0, g_offset = off;
        else { pthread_mutex_unlock(&g_lock); return true; }
    }
    if (g_ring_n == RING) {
        memmove(&g_ring[0], &g_ring[1], sizeof g_ring[0] * (RING - 1));
        memmove(&g_ring_time[0], &g_ring_time[1], sizeof g_ring_time[0] * (RING - 1));
        g_ring_n--;
    }
    g_ring[g_ring_n] = in;
    g_ring_time[g_ring_n++] = in.time_ms;
    pthread_mutex_unlock(&g_lock);
    static int test = -1;
    if (test < 0) test = getenv("LARECOMP_COOP_TEST") != NULL;
    if (test) {  // (each different list of pieces the other player's Link is drawn with, once)
        static uint32_t seen[64];
        static int nseen;
        uint32_t h = 2166136261u;
        for (int i = 0; i < in.nparts; i++) h = (h ^ (in.parts[i].vertices * 4 + in.parts[i].rigid * 2)) * 16777619u ^ in.parts[i].count ^ (uint32_t)in.parts[i].tag << 16;
        bool known = false;
        for (int i = 0; i < nseen; i++) known |= seen[i] == h;
        if (!known && nseen < 64) {
            seen[nseen++] = h;
            fprintf(stderr, "[coop] pose: their pieces:");
            for (int i = 0; i < in.nparts; i++) fprintf(stderr, " %u%s/%u@%d", in.parts[i].vertices, in.parts[i].rigid ? "r" : "", in.parts[i].count, (int16_t)in.parts[i].tag);
            fprintf(stderr, "\n");
        }
    }
    return true;
}

// Whether any of the other player's poses has arrived (until then, this player's own Link stands in for theirs).
bool coop_pose_have(void) {
    pthread_mutex_lock(&g_lock);
    bool have = g_ring_n > 0;
    pthread_mutex_unlock(&g_lock);
    return have;
}

static bool same_part(const Part *a, const Part *b) {
    return a->vertices == b->vertices && a->rigid == b->rigid && a->count == b->count && a->tag == b->tag;
}

// The same piece in another pose: the same name, and as many of that name before it (both eyes share one).
static const Part *same_in(const Pose *other, const Pose *p, int i) {
    int nth = 0;
    for (int k = 0; k < i; k++) nth += same_part(&p->parts[k], &p->parts[i]);
    for (int k = 0; k < other->nparts; k++)
        if (same_part(&other->parts[k], &p->parts[i]) && !nth--) return &other->parts[k];
    return NULL;
}

// Whether this player's own Link has a piece of this name (it'll want the other player's for itself).
static bool mine(uint32_t vertices, uint16_t tag, bool rigid, uint16_t count) {
    for (int i = 0; i < g_ready.nparts; i++) {
        const Part *q = &g_ready.parts[i];
        if (q->vertices == vertices && q->tag == tag && q->rigid == rigid && q->count == count) return true;
    }
    return false;
}

bool coop_pose_apply(uint64_t frame, uint64_t now_ms, uint32_t vertices, uint16_t tag, uint32_t alt, bool rigid, float *bones, float *rot, uint32_t count,
                     const float view[12], bool *hidden) {
    pthread_mutex_lock(&g_lock);
    bool ok = false;
    if (frame != g_used_frame) { g_used_frame = frame; memset(g_used, 0, sizeof g_used); }
    if (g_ring_n) {  // (however old: the other player's Link isn't drawn in first person, so nothing new comes then)
        // The two poses either side of the moment being played; before the first or past the last, that one alone
        uint32_t at = (uint32_t)((int64_t)now_ms - g_offset - COOP_POSE_DELAY_MS);
        int later = g_ring_n - 1, earlier = g_ring_n - 1;
        for (int i = 0; i < g_ring_n; i++) {
            if ((int32_t)(g_ring_time[i] - at) >= 0) { later = i; earlier = i ? i - 1 : i; break; }
        }
        const Pose *p = &g_ring[later], *e = &g_ring[earlier];
        float t = 1;
        if (later != earlier) t = (float)(int32_t)(at - g_ring_time[earlier]) / (float)(int32_t)(g_ring_time[later] - g_ring_time[earlier]);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        // Theirs of the same name; else one of the size given as a stand-in (a gripping hand for an open one), or the same size
        // under another name (their model elsewhere in memory), that isn't one this Link draws as itself.
        int pick = -1;
        for (int pass = 0; pass < 3 && pick < 0; pass++)
            for (int i = 0; i < p->nparts && pick < 0; i++) {
                const Part *q = &p->parts[i];
                if (g_used[i] || q->rigid != (uint8_t)rigid || q->count != count) continue;
                if (pass == 0 ? q->vertices == vertices && q->tag == tag
                              : (pass == 1 ? alt && q->vertices == alt : q->vertices == vertices) && !mine(q->vertices, q->tag, q->rigid, q->count))
                    pick = i;
            }
        for (int i = pick; i >= 0 && !ok; i = -1) {
            const Part *q = &p->parts[i];
            g_used[i] = 1;
            if (hidden) *hidden = q->hidden;  // (the latest's)
            // Blended part by part, not by bone number: bones that happen to coincide are shared differently from one
            // pose to the next. A part the earlier pose hasn't got appears at once.
            const Part *eq = e != p ? same_in(e, p, i) : q;
            float feet[3];
            for (int k = 0; k < 3; k++) feet[k] = e->feet[k] + (p->feet[k] - e->feet[k]) * t;
            for (uint32_t b = 0; b < count; b++) {
                const float *to = p->bones[p->slots[q->first + b]];
                const float *from = eq ? e->bones[e->slots[eq->first + b]] : to;
                float m[12];
                for (int k = 0; k < 12; k++) m[k] = from[k] + (to[k] - from[k]) * t;
                // (each pose's translations are from its own feet: blended with them, they stay on the body)
                if (eq) for (int k = 0; k < 3; k++) m[k * 4 + 3] = from[k * 4 + 3] + (to[k * 4 + 3] - from[k * 4 + 3]) * t;
                from_relative(bones + b * 12, rot ? rot + b * 12 : NULL, m, view, feet);
            }
            ok = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
