/* The 3D engine: geometry commands (GBATEK "DS 3D") and a software rasterizer.
 *
 * Commands arrive through the packed FIFO (0x04000400) or the per-command ports (0x04000440 + 4 * cmd) and run at
 * once. Polygons collect until SWAP_BUFFERS; the next VBlank rasterizes them into the framebuffer that BG0 of
 * engine A shows. Covered: matrices and stacks, all vertex formats, colours and lighting (diffuse, ambient,
 * emission, specular without the shininess table), texture coordinates (none / texcoord transform), polygon and
 * texture attributes, all seven texture formats, near-plane clipping, depth test, back/front culling, alpha test,
 * alpha blending. Not yet: toon/highlight, shadow polygons, fog, edge marking, anti-aliasing, translucent sorting. */
#include "internal.h"
#include <math.h>

typedef int32_t Mtx[16];          /* 4x4, 20.12 fixed point, row vectors: v' = v * M */

static Mtx proj, pos, vec, tex, clip;
static Mtx proj_stack[1], pos_stack[32], vec_stack[32], tex_stack[1];
static int mtx_mode, pos_sp, proj_sp, tex_sp, stack_error;

typedef struct {
    float x, y, z, w;             /* clip space */
    float r, g, b;                /* 0-31 */
    float s, t;                   /* texels */
} Vtx;

typedef struct {
    Vtx v[10];
    int n;
    uint32_t attr, texparam, palbase;
    int translucent;
} Poly;

#define MAX_POLYS 4096
static Poly polys[MAX_POLYS];
static int npolys;
static Vtx strip[4];
static int nstrip, prim, vtx_count;
static uint32_t cur_attr, pending_attr, texparam, palbase;
static int16_t vx, vy, vz;
static float cur_r = 31, cur_g = 31, cur_b = 31, cur_s, cur_t;
static int raw_s, raw_t;
static uint32_t diffuse = 0, ambient = 0, specular = 0, emission = 0;
static int32_t light_vec[4][3];
static uint16_t light_col[4];
static int swap_pending, wbuffer;
static uint8_t viewport[4] = {0, 0, 255, 191};

static uint32_t fb[256 * 192];
static uint32_t depth[256 * 192];

/* ------------------------------------------------------------------ matrices */

static void mtx_mul(Mtx out, const Mtx a, const Mtx b) {   /* out = a * b */
    Mtx r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t s = 0;
            for (int k = 0; k < 4; k++) s += (int64_t)a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = (int32_t)(s >> 12);
        }
    memcpy(out, r, sizeof r);
}

static void mtx_identity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    m[0] = m[5] = m[10] = m[15] = 1 << 12;
}

static void update_clip(void) { mtx_mul(clip, pos, proj); }

/* Applies m (multiplied in front) to the matrices the current mode selects */
static void apply(const Mtx m, int is_scale) {
    switch (mtx_mode) {
    case 0: mtx_mul(proj, m, proj); break;
    case 1: mtx_mul(pos, m, pos); break;
    case 2: mtx_mul(pos, m, pos); if (!is_scale) mtx_mul(vec, m, vec); break;
    case 3: mtx_mul(tex, m, tex); break;
    }
    update_clip();
}

static void load(const Mtx m) {
    switch (mtx_mode) {
    case 0: memcpy(proj, m, sizeof(Mtx)); break;
    case 1: memcpy(pos, m, sizeof(Mtx)); break;
    case 2: memcpy(pos, m, sizeof(Mtx)); memcpy(vec, m, sizeof(Mtx)); break;
    case 3: memcpy(tex, m, sizeof(Mtx)); break;
    }
    update_clip();
}

/* ------------------------------------------------------------------ vertices and polygons */

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static Vtx vlerp(const Vtx *a, const Vtx *b, float t) {
    Vtx r;
    r.x = lerpf(a->x, b->x, t); r.y = lerpf(a->y, b->y, t); r.z = lerpf(a->z, b->z, t); r.w = lerpf(a->w, b->w, t);
    r.r = lerpf(a->r, b->r, t); r.g = lerpf(a->g, b->g, t); r.b = lerpf(a->b, b->b, t);
    r.s = lerpf(a->s, b->s, t); r.t = lerpf(a->t, b->t, t);
    return r;
}

/* Clip a polygon against one plane (dot >= 0 is inside) */
static int clip_plane(Vtx *in, int n, Vtx *out, int axis, float sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        Vtx *a = &in[i], *b = &in[(i + 1) % n];
        float da = (axis == 0 ? a->x : axis == 1 ? a->y : a->z) * sign + a->w;
        float db = (axis == 0 ? b->x : axis == 1 ? b->y : b->z) * sign + b->w;
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) out[m++] = vlerp(a, b, da / (da - db));
        if (m >= 9) break;
    }
    return m;
}

static void emit_poly(Vtx *v, int n) {
    if (npolys >= MAX_POLYS) return;
    Vtx a[10], b[10];
    memcpy(a, v, sizeof(Vtx) * n);
    /* Near and far planes; x and y are cut by the viewport scissor when drawing */
    n = clip_plane(a, n, b, 2, 1);
    if (n < 3) return;
    n = clip_plane(b, n, a, 2, -1);
    if (n < 3) return;
    Poly *p = &polys[npolys++];
    memcpy(p->v, a, sizeof(Vtx) * n);
    p->n = n;
    p->attr = cur_attr;
    p->texparam = texparam;
    p->palbase = palbase;
    uint32_t alpha = (cur_attr >> 16) & 31, fmt = (texparam >> 26) & 7;
    p->translucent = (alpha && alpha < 31) || fmt == 1 || fmt == 6;
}

static void add_vertex(void) {
    Vtx v;
    float x = vx / 4096.0f, y = vy / 4096.0f, z = vz / 4096.0f;
    float m[16];
    for (int i = 0; i < 16; i++) m[i] = clip[i] / 4096.0f;
    v.x = x * m[0] + y * m[4] + z * m[8] + m[12];
    v.y = x * m[1] + y * m[5] + z * m[9] + m[13];
    v.z = x * m[2] + y * m[6] + z * m[10] + m[14];
    v.w = x * m[3] + y * m[7] + z * m[11] + m[15];
    v.r = cur_r; v.g = cur_g; v.b = cur_b;
    if (((texparam >> 30) & 3) == 3) {       /* vertex-source texture coordinates */
        float t[16];
        for (int i = 0; i < 16; i++) t[i] = tex[i] / 4096.0f;
        v.s = (x * t[0] + y * t[4] + z * t[8]) * 16 + raw_s / 16.0f;
        v.t = (x * t[1] + y * t[5] + z * t[9]) * 16 + raw_t / 16.0f;
    } else {
        v.s = cur_s; v.t = cur_t;
    }
    vtx_count++;
    switch (prim) {
    case 0:                                   /* triangles */
        strip[nstrip++] = v;
        if (nstrip == 3) { emit_poly(strip, 3); nstrip = 0; }
        break;
    case 1:                                   /* quads */
        strip[nstrip++] = v;
        if (nstrip == 4) { emit_poly(strip, 4); nstrip = 0; }
        break;
    case 2:                                   /* triangle strip, alternating winding */
        strip[nstrip++] = v;
        if (nstrip == 3) {
            if (vtx_count & 1) emit_poly(strip, 3);
            else { Vtx t[3] = {strip[1], strip[0], strip[2]}; emit_poly(t, 3); }
            strip[0] = strip[1]; strip[1] = strip[2]; nstrip = 2;
        }
        break;
    case 3:                                   /* quad strip */
        strip[nstrip++] = v;
        if (nstrip == 4) {
            Vtx q[4] = {strip[0], strip[1], strip[3], strip[2]};
            emit_poly(q, 4);
            strip[0] = strip[2]; strip[1] = strip[3]; nstrip = 2;
        }
        break;
    }
}

static void set_color(uint16_t c) { cur_r = c & 31; cur_g = (c >> 5) & 31; cur_b = (c >> 10) & 31; }

static void do_normal(uint32_t p) {
    float n[3];
    for (int i = 0; i < 3; i++) n[i] = ((int32_t)(p << (22 - 10 * i)) >> 22) / 512.0f;
    if (((texparam >> 30) & 3) == 2) {        /* normal-source texture coordinates */
        float t[16];
        for (int i = 0; i < 16; i++) t[i] = tex[i] / 4096.0f;
        cur_s = (n[0] * t[0] + n[1] * t[4] + n[2] * t[8]) * 16 + raw_s / 16.0f;
        cur_t = (n[0] * t[1] + n[1] * t[5] + n[2] * t[9]) * 16 + raw_t / 16.0f;
    }
    float tn[3];
    for (int i = 0; i < 3; i++) tn[i] = (n[0] * vec[i] + n[1] * vec[4 + i] + n[2] * vec[8 + i]) / 4096.0f;
    float r = emission & 31, g = (emission >> 5) & 31, b = (emission >> 10) & 31;
    for (int l = 0; l < 4; l++) {
        if (!(cur_attr & (1u << l))) continue;
        float lv[3];
        for (int i = 0; i < 3; i++) lv[i] = light_vec[l][i] / 512.0f;
        float d = -(lv[0] * tn[0] + lv[1] * tn[1] + lv[2] * tn[2]);
        if (d < 0) d = 0;
        float hz = 1 - lv[2], hlen = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + hz * hz);
        float sp = hlen > 0 ? -(lv[0] * tn[0] + lv[1] * tn[1] - hz * tn[2]) / hlen : 0;
        if (sp < 0) sp = 0;
        sp = sp * sp;
        uint16_t lc = light_col[l];
        float lr = lc & 31, lg = (lc >> 5) & 31, lb = (lc >> 10) & 31;
        r += lr * ((diffuse & 31) * d + (ambient & 31) + (specular & 31) * sp) / 31;
        g += lg * (((diffuse >> 5) & 31) * d + ((ambient >> 5) & 31) + ((specular >> 5) & 31) * sp) / 31;
        b += lb * (((diffuse >> 10) & 31) * d + ((ambient >> 10) & 31) + ((specular >> 10) & 31) * sp) / 31;
    }
    cur_r = r > 31 ? 31 : r; cur_g = g > 31 ? 31 : g; cur_b = b > 31 ? 31 : b;
}

static void do_texcoord(uint32_t p) {
    raw_s = (int16_t)(p & 0xffff); raw_t = (int16_t)(p >> 16);
    if (((texparam >> 30) & 3) == 1) {        /* texcoord-source: (s, t, 1/16, 1/16) * texture matrix */
        float t[16];
        for (int i = 0; i < 16; i++) t[i] = tex[i] / 4096.0f;
        float s0 = raw_s / 16.0f, t0 = raw_t / 16.0f;
        cur_s = s0 * t[0] + t0 * t[4] + t[8] / 16 + t[12] / 16;
        cur_t = s0 * t[1] + t0 * t[5] + t[9] / 16 + t[13] / 16;
    } else {
        cur_s = raw_s / 16.0f; cur_t = raw_t / 16.0f;
    }
}

/* ------------------------------------------------------------------ commands */

static const uint8_t nparams[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12, [0x18] = 16,
    [0x19] = 12, [0x1a] = 9, [0x1b] = 3, [0x1c] = 3, [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1,
    [0x25] = 1, [0x26] = 1, [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2a] = 1, [0x2b] = 1, [0x30] = 1, [0x31] = 1,
    [0x32] = 1, [0x33] = 1, [0x34] = 32, [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2,
    [0x72] = 1,
};

static void run(uint8_t cmd, const uint32_t *p) {
    Mtx m;
    switch (cmd) {
    case 0x10: mtx_mode = p[0] & 3; break;
    case 0x11:                                                  /* PUSH */
        if (mtx_mode == 0) { memcpy(proj_stack[0], proj, sizeof(Mtx)); proj_sp = 1; }
        else if (mtx_mode == 3) { memcpy(tex_stack[0], tex, sizeof(Mtx)); tex_sp = 1; }
        else {
            if (pos_sp >= 31) stack_error = 1;
            memcpy(pos_stack[pos_sp & 31], pos, sizeof(Mtx));
            memcpy(vec_stack[pos_sp & 31], vec, sizeof(Mtx));
            pos_sp = (pos_sp + 1) & 63;
        }
        break;
    case 0x12: {                                                /* POP */
        if (mtx_mode == 0) { memcpy(proj, proj_stack[0], sizeof(Mtx)); proj_sp = 0; }
        else if (mtx_mode == 3) { memcpy(tex, tex_stack[0], sizeof(Mtx)); tex_sp = 0; }
        else {
            int n = (int)((int32_t)(p[0] << 26) >> 26);
            pos_sp = (pos_sp - n) & 63;
            memcpy(pos, pos_stack[pos_sp & 31], sizeof(Mtx));
            memcpy(vec, vec_stack[pos_sp & 31], sizeof(Mtx));
        }
        update_clip();
        break;
    }
    case 0x13:                                                  /* STORE */
        if (mtx_mode == 0) memcpy(proj_stack[0], proj, sizeof(Mtx));
        else if (mtx_mode == 3) memcpy(tex_stack[0], tex, sizeof(Mtx));
        else { memcpy(pos_stack[p[0] & 31], pos, sizeof(Mtx)); memcpy(vec_stack[p[0] & 31], vec, sizeof(Mtx)); }
        break;
    case 0x14:                                                  /* RESTORE */
        if (mtx_mode == 0) memcpy(proj, proj_stack[0], sizeof(Mtx));
        else if (mtx_mode == 3) memcpy(tex, tex_stack[0], sizeof(Mtx));
        else { memcpy(pos, pos_stack[p[0] & 31], sizeof(Mtx)); memcpy(vec, vec_stack[p[0] & 31], sizeof(Mtx)); }
        update_clip();
        break;
    case 0x15: mtx_identity(m); load(m); break;
    case 0x16: for (int i = 0; i < 16; i++) m[i] = (int32_t)p[i]; load(m); break;
    case 0x17:
        mtx_identity(m);
        for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) m[r * 4 + c] = (int32_t)p[r * 3 + c];
        load(m);
        break;
    case 0x18: for (int i = 0; i < 16; i++) m[i] = (int32_t)p[i]; apply(m, 0); break;
    case 0x19:
        mtx_identity(m);
        for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) m[r * 4 + c] = (int32_t)p[r * 3 + c];
        apply(m, 0);
        break;
    case 0x1a:
        mtx_identity(m);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 4 + c] = (int32_t)p[r * 3 + c];
        apply(m, 0);
        break;
    case 0x1b:
        mtx_identity(m);
        m[0] = (int32_t)p[0]; m[5] = (int32_t)p[1]; m[10] = (int32_t)p[2];
        apply(m, 1);
        break;
    case 0x1c:
        mtx_identity(m);
        m[12] = (int32_t)p[0]; m[13] = (int32_t)p[1]; m[14] = (int32_t)p[2];
        apply(m, 0);
        break;
    case 0x20: set_color((uint16_t)p[0]); break;
    case 0x21: do_normal(p[0]); break;
    case 0x22: do_texcoord(p[0]); break;
    case 0x23: vx = (int16_t)p[0]; vy = (int16_t)(p[0] >> 16); vz = (int16_t)p[1]; add_vertex(); break;
    case 0x24:
        vx = (int16_t)((p[0] & 0x3ff) << 6); vy = (int16_t)(((p[0] >> 10) & 0x3ff) << 6);
        vz = (int16_t)(((p[0] >> 20) & 0x3ff) << 6);
        add_vertex();
        break;
    case 0x25: vx = (int16_t)p[0]; vy = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x26: vx = (int16_t)p[0]; vz = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x27: vy = (int16_t)p[0]; vz = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x28: {                                                /* VTX_DIFF: s0.9 differences, scaled by 1/8 */
        int dx = (int32_t)(p[0] << 22) >> 22, dy = (int32_t)(p[0] << 12) >> 22, dz = (int32_t)(p[0] << 2) >> 22;
        vx = (int16_t)(vx + dx); vy = (int16_t)(vy + dy); vz = (int16_t)(vz + dz);
        add_vertex();
        break;
    }
    case 0x29: pending_attr = p[0]; break;
    case 0x2a: texparam = p[0]; break;
    case 0x2b: palbase = p[0] & 0x1fff; break;
    case 0x30:
        diffuse = p[0] & 0x7fff; ambient = (p[0] >> 16) & 0x7fff;
        if (p[0] & 0x8000) set_color((uint16_t)diffuse);
        break;
    case 0x31: specular = p[0] & 0x7fff; emission = (p[0] >> 16) & 0x7fff; break;
    case 0x32: {
        int l = p[0] >> 30;
        float lv[3];
        for (int i = 0; i < 3; i++) lv[i] = (float)((int32_t)(p[0] << (22 - 10 * i)) >> 22);
        for (int i = 0; i < 3; i++)
            light_vec[l][i] = (int32_t)((lv[0] * vec[i] + lv[1] * vec[4 + i] + lv[2] * vec[8 + i]) / 4096.0f);
        break;
    }
    case 0x33: light_col[p[0] >> 30] = (uint16_t)(p[0] & 0x7fff); break;
    case 0x40: prim = p[0] & 3; nstrip = 0; vtx_count = 0; cur_attr = pending_attr; break;
    case 0x41: break;
    case 0x50: swap_pending = 1; wbuffer = (p[0] >> 1) & 1; break;
    case 0x60:
        viewport[0] = (uint8_t)p[0]; viewport[1] = (uint8_t)(p[0] >> 8);
        viewport[2] = (uint8_t)(p[0] >> 16); viewport[3] = (uint8_t)(p[0] >> 24);
        break;
    case 0x70: set_io32(0x600, io32(0x600) | 2); break;           /* box test: say it's visible */
    case 0x71: {
        vx = (int16_t)p[0]; vy = (int16_t)(p[0] >> 16); vz = (int16_t)p[1];
        for (int i = 0; i < 4; i++)
            set_io32(0x620 + i * 4, (uint32_t)(((int64_t)vx * clip[i] + (int64_t)vy * clip[4 + i] +
                                                 (int64_t)vz * clip[8 + i] + ((int64_t)clip[12 + i] << 12)) >> 12));
        break;
    }
    case 0x72: {
        float n[3];
        for (int i = 0; i < 3; i++) n[i] = (float)((int32_t)(p[0] << (22 - 10 * i)) >> 22);
        for (int i = 0; i < 3; i++)
            set_io16(0x630 + i * 2, (uint16_t)(int16_t)((n[0] * vec[i] + n[1] * vec[4 + i] + n[2] * vec[8 + i]) / 4096.0f * 8));
        break;
    }
    default: break;
    }
}

/* Packed FIFO: a word of up to four command bytes, then their parameters in order */
static uint8_t fifo_cmds[4];
static int fifo_ncmds, fifo_cur;
static uint32_t params[32];
static int nparam;

static void fifo_next(void) {
    while (fifo_cur < fifo_ncmds && nparams[fifo_cmds[fifo_cur]] == 0) {
        run(fifo_cmds[fifo_cur], params);
        fifo_cur++;
    }
    if (fifo_cur >= fifo_ncmds) fifo_ncmds = 0;
}

static void fifo_word(uint32_t v) {
    if (fifo_ncmds == 0) {
        if (v == 0) return;
        fifo_ncmds = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t c = (uint8_t)(v >> (8 * i));
            if (c || i == 0) fifo_cmds[fifo_ncmds++] = c;
            else break;
        }
        fifo_cur = 0;
        nparam = 0;
        fifo_next();
        return;
    }
    params[nparam++] = v;
    if (nparam >= nparams[fifo_cmds[fifo_cur]]) {
        run(fifo_cmds[fifo_cur], params);
        fifo_cur++;
        nparam = 0;
        fifo_next();
    }
}

static uint32_t port_params[256][32];
static uint8_t port_n[256];

void gpu3d_write(uint32_t off, uint32_t v) {
    if (off < 0x400) { set_io32(off, v); return; }
    if (off < 0x440) { fifo_word(v); return; }
    if (off < 0x600) {
        uint8_t cmd = (uint8_t)((off - 0x400) / 4);
        int np = nparams[cmd];
        if (np == 0) { run(cmd, &v); return; }
        port_params[cmd][port_n[cmd]++] = v;
        if (port_n[cmd] >= np) { run(cmd, port_params[cmd]); port_n[cmd] = 0; }
        return;
    }
    if (off == 0x600) {                                        /* GXSTAT: acknowledge the stack error */
        if (v & 0x8000) stack_error = 0;
        set_io32(0x600, (io32(0x600) & ~0xc0000000u) | (v & 0xc0000000u));
    }
}

uint32_t gpu3d_read(uint32_t off) {
    if (off == 0x600) {
        /* FIFO empty and less than half full, not busy; stack levels; box test result kept from the test */
        return (io32(0x600) & 0xc0000002u) | 3u << 25 | (uint32_t)(pos_sp & 31) << 8 |
               (uint32_t)(proj_sp & 1) << 13 | (uint32_t)stack_error << 15;
    }
    if (off == 0x604) return (uint32_t)npolys | (uint32_t)vtx_count << 16;
    if (off >= 0x640 && off < 0x680) return (uint32_t)clip[(off - 0x640) / 4];
    if (off >= 0x680 && off < 0x6a4) {
        int i = (off - 0x680) / 4;
        return (uint32_t)vec[(i / 3) * 4 + i % 3];
    }
    return io32(off);
}

/* ------------------------------------------------------------------ textures */

static uint8_t tex_byte(uint32_t addr) {
    uint8_t *s = rc_tex_slot((addr >> 17) & 3);
    return s ? s[addr & 0x1ffff] : 0;
}
static uint16_t pal_half(uint32_t addr) {
    uint8_t *s = rc_texpal_slot((addr >> 14) & 7);
    if (!s) return 0;
    uint16_t v;
    memcpy(&v, s + (addr & 0x3fff), 2);
    return v;
}

/* Returns 15-bit colour with alpha 0-31 in bits 16-20 */
static uint32_t texel(uint32_t tp, uint32_t pb, int s, int t) {
    int w = 8 << ((tp >> 20) & 7), h = 8 << ((tp >> 23) & 7);
    if (tp & (1u << 16)) {
        if (tp & (1u << 18)) { int m = s & (2 * w - 1); s = m >= w ? 2 * w - 1 - m : m; }
        else s &= w - 1;
    } else s = s < 0 ? 0 : s >= w ? w - 1 : s;
    if (tp & (1u << 17)) {
        if (tp & (1u << 19)) { int m = t & (2 * h - 1); t = m >= h ? 2 * h - 1 - m : m; }
        else t &= h - 1;
    } else t = t < 0 ? 0 : t >= h ? h - 1 : t;
    uint32_t base = (tp & 0xffff) * 8, fmt = (tp >> 26) & 7, i = (uint32_t)(t * w + s);
    int c0 = (tp >> 29) & 1;
    switch (fmt) {
    case 1: { uint8_t b = tex_byte(base + i); uint32_t a = b >> 5; return pal_half(pb * 16 + (b & 31) * 2) | ((a * 4 + a / 2) << 16); }
    case 2: { uint8_t b = (tex_byte(base + i / 4) >> ((i & 3) * 2)) & 3; return c0 && !b ? 0 : pal_half(pb * 8 + b * 2) | 31u << 16; }
    case 3: { uint8_t b = (tex_byte(base + i / 2) >> ((i & 1) * 4)) & 15; return c0 && !b ? 0 : pal_half(pb * 16 + b * 2) | 31u << 16; }
    case 4: { uint8_t b = tex_byte(base + i); return c0 && !b ? 0 : pal_half(pb * 16 + b * 2) | 31u << 16; }
    case 5: {                                                   /* 4x4 compressed */
        uint32_t blk = (uint32_t)((t / 4) * (w / 4) + s / 4);
        uint32_t addr = base + blk * 4;
        uint8_t bits = (tex_byte(addr + (t & 3)) >> ((s & 3) * 2)) & 3;
        uint32_t slot = (addr >> 17) & 3;
        uint32_t iaddr = 0x20000 + ((addr & 0x1ffff) >> 1) + (slot == 2 ? 0x10000 : 0);
        uint16_t pi = (uint16_t)(tex_byte(iaddr) | tex_byte(iaddr + 1) << 8);
        uint32_t pa = pb * 16 + (pi & 0x3fff) * 4;
        int mode = pi >> 14;
        uint16_t p0 = pal_half(pa), p1 = pal_half(pa + 2);
        switch (bits) {
        case 0: return p0 | 31u << 16;
        case 1: return p1 | 31u << 16;
        case 2:
            if (mode == 0 || mode == 2) return pal_half(pa + 4) | 31u << 16;
            if (mode == 1) {
                uint32_t r = ((p0 & 31) + (p1 & 31)) / 2, g = (((p0 >> 5) & 31) + ((p1 >> 5) & 31)) / 2,
                         b = (((p0 >> 10) & 31) + ((p1 >> 10) & 31)) / 2;
                return r | g << 5 | b << 10 | 31u << 16;
            } else {
                uint32_t r = ((p0 & 31) * 5 + (p1 & 31) * 3) / 8, g = (((p0 >> 5) & 31) * 5 + ((p1 >> 5) & 31) * 3) / 8,
                         b = (((p0 >> 10) & 31) * 5 + ((p1 >> 10) & 31) * 3) / 8;
                return r | g << 5 | b << 10 | 31u << 16;
            }
        default:
            if (mode == 2) return pal_half(pa + 6) | 31u << 16;
            if (mode == 3) {
                uint32_t r = ((p0 & 31) * 3 + (p1 & 31) * 5) / 8, g = (((p0 >> 5) & 31) * 3 + ((p1 >> 5) & 31) * 5) / 8,
                         b = (((p0 >> 10) & 31) * 3 + ((p1 >> 10) & 31) * 5) / 8;
                return r | g << 5 | b << 10 | 31u << 16;
            }
            return 0;
        }
    }
    case 6: { uint8_t b = tex_byte(base + i); return pal_half(pb * 16 + (b & 7) * 2) | (uint32_t)(b >> 3) << 16; }
    case 7: {
        uint16_t c = (uint16_t)(tex_byte(base + i * 2) | tex_byte(base + i * 2 + 1) << 8);
        return (c & 0x7fff) | (c & 0x8000 ? 31u << 16 : 0);
    }
    default: return 0x7fff | 31u << 16;
    }
}

/* ------------------------------------------------------------------ rasterizer */

typedef struct { float x, y, z, iw, r, g, b, s, t; } SV;      /* screen-space vertex, attributes divided by w */

static void draw_triangle(const Poly *p, SV *a, SV *b, SV *c) {
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    /* Front faces are clockwise on screen; bits 6 and 7 of the polygon attributes enable back and front */
    int front = area < 0;
    if (front && !(p->attr & 0x80)) return;
    if (!front && !(p->attr & 0x40)) return;
    int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 255) x1 = 255;
    if (y1 > 191) y1 = 191;
    uint32_t alpha_attr = (p->attr >> 16) & 31;
    uint32_t fmt = (p->texparam >> 26) & 7, mode = (p->attr >> 4) & 3;
    uint16_t disp = io16(0x60);
    int textures = (disp & 1) && fmt;
    int alpha_test = disp & 4, blending = disp & 8;
    uint32_t alpha_ref = io16(0x340) & 31;
    int depth_equal = p->attr & 0x4000, write_depth = !p->translucent || (p->attr & 0x800);
    float inv = 1.0f / area;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
            if (iw <= 0) continue;
            float wv = 1 / iw;
            uint32_t z;
            if (wbuffer) z = (uint32_t)fminf(wv * 4096.0f, 16777215.0f);
            else {
                float zz = (w0 * a->z + w1 * b->z + w2 * c->z);
                z = (uint32_t)fminf(fmaxf((zz * 0.5f + 0.5f) * 16777215.0f, 0), 16777215.0f);
            }
            int i = y * 256 + x;
            if (depth_equal ? (z > depth[i] + 0x200 || z + 0x200 < depth[i]) : z >= depth[i]) continue;
            float r = (w0 * a->r + w1 * b->r + w2 * c->r) * wv, g = (w0 * a->g + w1 * b->g + w2 * c->g) * wv,
                  bl = (w0 * a->b + w1 * b->b + w2 * c->b) * wv;
            int cr = (int)r, cg = (int)g, cb = (int)bl;
            int ca = alpha_attr ? (int)alpha_attr : 31;
            if (textures) {
                float s = (w0 * a->s + w1 * b->s + w2 * c->s) * wv, t = (w0 * a->t + w1 * b->t + w2 * c->t) * wv;
                uint32_t tx = texel(p->texparam, p->palbase, (int)floorf(s), (int)floorf(t));
                int ta = (tx >> 16) & 31;
                if (!ta) continue;
                int tr = tx & 31, tg = (tx >> 5) & 31, tb = (tx >> 10) & 31;
                if (mode == 1) {                                    /* decal */
                    cr = (tr * ta + cr * (31 - ta)) / 31; cg = (tg * ta + cg * (31 - ta)) / 31;
                    cb = (tb * ta + cb * (31 - ta)) / 31;
                } else {                                            /* modulation */
                    cr = ((tr + 1) * (cr + 1) - 1) / 32; cg = ((tg + 1) * (cg + 1) - 1) / 32;
                    cb = ((tb + 1) * (cb + 1) - 1) / 32;
                    ca = ((ta + 1) * (ca + 1) - 1) / 32;
                }
            }
            if (alpha_test && (uint32_t)ca <= alpha_ref) continue;
            if (cr > 31) cr = 31;
            if (cg > 31) cg = 31;
            if (cb > 31) cb = 31;
            uint32_t dst = fb[i], da = (dst >> 16) & 31;
            if (blending && ca < 31 && da) {
                int dr = dst & 31, dg = (dst >> 5) & 31, db = (dst >> 10) & 31;
                cr = (cr * (ca + 1) + dr * (31 - ca)) / 32; cg = (cg * (ca + 1) + dg * (31 - ca)) / 32;
                cb = (cb * (ca + 1) + db * (31 - ca)) / 32;
                ca = ca > (int)da ? ca : (int)da;
            }
            fb[i] = (uint32_t)(cr | cg << 5 | cb << 10) | (uint32_t)(ca ? ca : 1) << 16;
            if (write_depth) depth[i] = z;
        }
    }
}

static void draw_poly(const Poly *p) {
    SV sv[10];
    float vx0 = viewport[0], vy0 = viewport[1], vw = (float)(viewport[2] - viewport[0] + 1),
          vh = (float)(viewport[3] - viewport[1] + 1);
    for (int i = 0; i < p->n; i++) {
        const Vtx *v = &p->v[i];
        float w = v->w;
        if (w == 0) w = 0.00001f;
        float iw = 1 / w;
        sv[i].x = (v->x * iw + 1) * vw / 2 + vx0;
        sv[i].y = 192 - ((v->y * iw + 1) * vh / 2 + vy0);
        sv[i].z = v->z * iw;
        sv[i].iw = iw;
        sv[i].r = v->r * iw; sv[i].g = v->g * iw; sv[i].b = v->b * iw;
        sv[i].s = v->s * iw; sv[i].t = v->t * iw;
    }
    for (int i = 1; i + 1 < p->n; i++) draw_triangle(p, &sv[0], &sv[i], &sv[i + 1]);
}

void gpu3d_swap(void) {
    if (!swap_pending) return;
    swap_pending = 0;
    uint32_t cc = io32(0x350);
    uint32_t clear = (cc & 0x7fff) | ((cc >> 16) & 31) << 16;
    uint32_t cd = (io16(0x354) & 0x7fff) * 0x200 + 0x1ff;
    for (int i = 0; i < 256 * 192; i++) { fb[i] = clear; depth[i] = cd; }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < npolys; i++)
            if (polys[i].translucent == pass) draw_poly(&polys[i]);
    npolys = 0;
}

const uint32_t *gpu3d_framebuffer(void) { return fb; }
