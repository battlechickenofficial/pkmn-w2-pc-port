/* The 3D engine: geometry commands (GBATEK "DS 3D") and a software rasterizer.
 *
 * Commands arrive through the packed FIFO (0x04000400) or the per-command ports (0x04000440 + 4 * cmd) and run at
 * once. Polygons collect until SWAP_BUFFERS; the next VBlank rasterizes them into the framebuffer that BG0 of
 * engine A shows. Covered: matrices and stacks, all vertex formats, colours and lighting (diffuse, ambient,
 * emission, specular with the shininess table), texture coordinates (none / texcoord transform), polygon and
 * texture attributes, all seven texture formats, near-plane clipping, depth test, back/front culling, alpha test,
 * alpha blending, toon/highlight, shadow polygons, translucent sorting, edge marking, fog. Anti-aliasing is an
 * approximation (edges softened toward the neighbour behind them). */
#include "internal.h"
#include <math.h>
#include <stdlib.h>

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
static uint8_t shininess[128];
static int use_shininess;
static int32_t light_vec[4][3];
static uint16_t light_col[4];
static int swap_pending, wbuffer, sort_manual;
static uint8_t viewport[4] = {0, 0, 255, 191};

static uint32_t fb[GPU3D_W * 192];
static uint32_t depth[GPU3D_W * 192];

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
    p->translucent = (alpha && alpha < 31) || fmt == 1 || fmt == 6 || ((cur_attr >> 4) & 3) == 3;
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
        if (use_shininess) sp = shininess[(int)(sp * 127) & 127] / 255.0f;
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
    case 0x31: specular = p[0] & 0x7fff; use_shininess = (p[0] >> 15) & 1; emission = (p[0] >> 16) & 0x7fff; break;
    case 0x34: for (int i = 0; i < 32; i++) for (int k = 0; k < 4; k++) shininess[i * 4 + k] = (uint8_t)(p[i] >> (8 * k)); break;
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
    case 0x50: swap_pending = 1; sort_manual = p[0] & 1; wbuffer = (p[0] >> 1) & 1; break;
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

/* Per-pixel attributes besides colour and depth */
static uint8_t opaque_id[GPU3D_W * 192];     /* polygon ID of the opaque pixel, for edge marking and shadows */
static uint8_t trans_id[GPU3D_W * 192];      /* ID of the last translucent polygon drawn here (0xff: none) */
static uint8_t fog_flag[GPU3D_W * 192];
static uint8_t stencil[GPU3D_W * 192];

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
    if (x1 > GPU3D_W - 1) x1 = GPU3D_W - 1;
    if (y1 > 191) y1 = 191;
    uint32_t alpha_attr = (p->attr >> 16) & 31;
    uint32_t fmt = (p->texparam >> 26) & 7, mode = (p->attr >> 4) & 3;
    uint8_t id = (uint8_t)((p->attr >> 24) & 63);
    uint16_t disp = io16(0x60);
    int textures = (disp & 1) && fmt;
    int highlight = disp & 2, alpha_test = disp & 4, blending = disp & 8;
    uint32_t alpha_ref = io16(0x340) & 31;
    int depth_equal = p->attr & 0x4000, write_depth = !p->translucent || (p->attr & 0x800);
    int shadow = mode == 3, fog = (p->attr >> 15) & 1;
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
            int i = y * GPU3D_W + x;
            int pass = depth_equal ? !(z > depth[i] + 0x200 || z + 0x200 < depth[i]) : z < depth[i];
            /* Shadow polygons: ID 0 marks where the shadow volume is hidden (fails the depth test); the others
             * draw there, except on the polygon that casts them (same ID) */
            if (shadow && id == 0) { if (!pass) stencil[i] = 1; continue; }
            if (!pass) continue;
            if (shadow) {
                if (!stencil[i] || opaque_id[i] == id) continue;
                stencil[i] = 0;
            }
            if (p->translucent && trans_id[i] == id) continue;   /* same translucent ID: no blending onto itself */
            float r = (w0 * a->r + w1 * b->r + w2 * c->r) * wv, g = (w0 * a->g + w1 * b->g + w2 * c->g) * wv,
                  bl = (w0 * a->b + w1 * b->b + w2 * c->b) * wv;
            int cr = (int)r, cg = (int)g, cb = (int)bl;
            int ca = alpha_attr ? (int)alpha_attr : 31;
            int hr = 0, hg = 0, hb = 0;
            if (mode == 2) {
                /* Toon: the vertex colour's red picks a toon table entry; highlight adds it after texturing */
                uint16_t toon = io16(0x380 + (uint32_t)(cr & 31) * 2);
                if (highlight) { hr = toon & 31; hg = (toon >> 5) & 31; hb = (toon >> 10) & 31; }
                else { cr = toon & 31; cg = (toon >> 5) & 31; cb = (toon >> 10) & 31; }
            }
            if (textures) {
                float s = (w0 * a->s + w1 * b->s + w2 * c->s) * wv, t = (w0 * a->t + w1 * b->t + w2 * c->t) * wv;
                uint32_t tx = texel(p->texparam, p->palbase, (int)floorf(s), (int)floorf(t));
                int ta = (tx >> 16) & 31;
                if (!ta) continue;
                int tr = tx & 31, tg = (tx >> 5) & 31, tb = (tx >> 10) & 31;
                if (mode == 1) {                                    /* decal */
                    cr = (tr * ta + cr * (31 - ta)) / 31; cg = (tg * ta + cg * (31 - ta)) / 31;
                    cb = (tb * ta + cb * (31 - ta)) / 31;
                } else {                                            /* modulation (also toon/highlight, shadow) */
                    cr = ((tr + 1) * (cr + 1) - 1) / 32; cg = ((tg + 1) * (cg + 1) - 1) / 32;
                    cb = ((tb + 1) * (cb + 1) - 1) / 32;
                    ca = ((ta + 1) * (ca + 1) - 1) / 32;
                }
            }
            cr += hr; cg += hg; cb += hb;
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
            if (p->translucent) { trans_id[i] = id; fog_flag[i] &= (uint8_t)fog; }
            else { opaque_id[i] = id; fog_flag[i] = (uint8_t)fog; }
        }
    }
}

static void project(const Poly *p, SV *sv) {
    float vx0 = viewport[0], vy0 = viewport[1], vw = (float)(viewport[2] - viewport[0] + 1),
          vh = (float)(viewport[3] - viewport[1] + 1);
    for (int i = 0; i < p->n; i++) {
        const Vtx *v = &p->v[i];
        float w = v->w;
        if (w == 0) w = 0.00001f;
        float iw = 1 / w;
        sv[i].x = (v->x * iw + 1) * vw / 2 + vx0 + GPU3D_OFS;     /* widescreen margin on the left */
        sv[i].y = 192 - ((v->y * iw + 1) * vh / 2 + vy0);
        sv[i].z = v->z * iw;
        sv[i].iw = iw;
        sv[i].r = v->r * iw; sv[i].g = v->g * iw; sv[i].b = v->b * iw;
        sv[i].s = v->s * iw; sv[i].t = v->t * iw;
    }
}

static void draw_poly(const Poly *p) {
    SV sv[10];
    project(p, sv);
    for (int i = 1; i + 1 < p->n; i++) draw_triangle(p, &sv[0], &sv[i], &sv[i + 1]);
}

/* Translucent polygons are drawn bottom-most Y first unless SWAP_BUFFERS asked for manual order */
static float sort_key[MAX_POLYS];
static int cmp_translucent(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    if (sort_key[a] != sort_key[b]) return sort_key[a] < sort_key[b] ? -1 : 1;
    return a - b;
}

static uint16_t blend15(uint16_t a, uint16_t b, int wa) {     /* wa of 32 parts a */
    int r = ((a & 31) * wa + (b & 31) * (32 - wa)) / 32, g = (((a >> 5) & 31) * wa + ((b >> 5) & 31) * (32 - wa)) / 32,
        bl = (((a >> 10) & 31) * wa + ((b >> 10) & 31) * (32 - wa)) / 32;
    return (uint16_t)(r | g << 5 | bl << 10);
}

/* After all polygons: edge marking (DISP3DCNT bit 5), anti-aliasing (bit 4), fog (bit 7) */
static void finish_frame(uint8_t clear_id) {
    uint16_t disp = io16(0x60);
    static uint32_t src[GPU3D_W * 192];
    if (disp & 0x30) {
        memcpy(src, fb, sizeof fb);
        static const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < GPU3D_W; x++) {
                int i = y * GPU3D_W + x;
                if (!((src[i] >> 16) & 31)) continue;
                /* An edge: a neighbour with another polygon ID lies behind this pixel */
                int edge = -1;
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k], ny = y + dy[k];
                    uint8_t nid = clear_id;
                    uint32_t nd = 0xffffff;
                    if (nx >= 0 && nx < GPU3D_W && ny >= 0 && ny < 192) { nid = opaque_id[ny * GPU3D_W + nx]; nd = depth[ny * GPU3D_W + nx]; }
                    if (nid != opaque_id[i] && depth[i] < nd) { edge = k; break; }
                }
                if (edge < 0) continue;
                uint32_t a = fb[i] & 0x1f0000;
                if (disp & 0x20) {
                    fb[i] = io16(0x330 + (uint32_t)(opaque_id[i] >> 3) * 2) | a;
                } else {                                                  /* anti-aliasing: soften the edge */
                    int nx = x + dx[edge], ny = y + dy[edge];
                    if (nx >= 0 && nx < GPU3D_W && ny >= 0 && ny < 192 && ((src[ny * GPU3D_W + nx] >> 16) & 31))
                        fb[i] = blend15((uint16_t)src[i], (uint16_t)src[ny * GPU3D_W + nx], 20) | a;
                }
            }
    }
    if (disp & 0x80) {
        /* Fog: density from the 32-entry table at 0x04000360, indexed by depth past FOG_OFFSET in steps of
         * 0x400 >> FOG_SHIFT, interpolated; colour and alpha move toward FOG_COLOR (only alpha with bit 6) */
        uint32_t fc = io32(0x358);
        uint16_t fog_color = (uint16_t)(fc & 0x7fff);
        int fog_alpha = (fc >> 16) & 31, alpha_only = disp & 0x40, shift = (disp >> 8) & 15;
        int offset = io16(0x35c) & 0x7fff, step = 0x400 >> shift;
        for (int i = 0; i < GPU3D_W * 192; i++) {
            if (!fog_flag[i] || !step) continue;
            int z15 = (int)(depth[i] >> 9), d;
            if (z15 <= offset) d = rc_io[0x360] & 0x7f;
            else {
                int pos = (z15 - offset) / step, frac = ((z15 - offset) % step) * 128 / step;
                if (pos >= 31) d = rc_io[0x37f] & 0x7f;
                else {
                    int d0 = rc_io[0x360 + pos] & 0x7f, d1 = rc_io[0x361 + pos] & 0x7f;
                    d = d0 + (d1 - d0) * frac / 128;
                }
            }
            if (d >= 127) d = 128;
            uint32_t p = fb[i];
            int a = (p >> 16) & 31;
            uint16_t c = (uint16_t)(p & 0x7fff);
            if (!alpha_only) c = blend15(fog_color, c, d / 4);
            a = (fog_alpha * d + a * (128 - d)) / 128;
            fb[i] = c | (uint32_t)a << 16;
        }
    }
}

/* B2W2_LOG_3D=<frame>: list that frame's polygons in the log */
static void log_polys(void) {
    static int64_t want = -2;
    if (want == -2) { const char *e = getenv("B2W2_LOG_3D"); want = e ? atoll(e) : -1; }
    if (want < 0 || (int64_t)rc_frames != want) return;
    rc_logf("3D frame %llu: %d polygons, DISP3DCNT %04x, lights %04x %04x %04x %04x, dif/amb %04x/%04x spe/emi %04x/%04x\n",
            (unsigned long long)rc_frames, npolys, io16(0x60), light_col[0], light_col[1], light_col[2], light_col[3],
            diffuse, ambient, specular, emission);
    for (uint32_t pb = 0; pb < 0x6000; pb += 0x1000) {
        rc_logf("  texpal %05x:", pb);
        for (int k = 0; k < 8; k++) rc_logf(" %04x", pal_half(pb + (uint32_t)k * 2));
        rc_logf("\n");
    }
    rc_logf("  VRAMCNT E %02x F %02x G %02x  A: DISPCNT %08x BLDCNT %04x BLDALPHA %04x BLDY %04x WIN %04x/%04x BG0CNT %04x\n",
            rc_io[0x244], rc_io[0x245], rc_io[0x246], io32(0), io16(0x50), io16(0x52), io16(0x54), io16(0x48), io16(0x4a),
            io16(8));
    {
        int lit = 0, black = 0;
        for (int i = 0; i < GPU3D_W * 192; i++) {
            uint32_t p = fb[i];
            if ((p >> 16) & 31) { lit++; if (!(p & 0x7fff)) black++; }
        }
        rc_logf("  previous 3D frame: %d pixels drawn, %d of them black\n", lit, black);
    }
    if (npolys) {
        rc_logf("  texels of poly 0:");
        for (int k = 0; k < 8; k++) rc_logf(" %06x", texel(polys[0].texparam, polys[0].palbase, 64 + k * 2, 104 + k));
        rc_logf("  slot3 %p\n", (void *)rc_tex_slot(3));
    }
    for (int i = 0; i < npolys && i < 40; i++) {
        const Poly *p = &polys[i];
        rc_logf("  poly %d: attr %08x tex %08x pal %04x n %d  v0 xyzw %.4f %.4f %.4f %.4f rgb %.0f %.0f %.0f st %.1f %.1f\n", i, p->attr, p->texparam,
                p->palbase, p->n, p->v[0].x, p->v[0].y, p->v[0].z, p->v[0].w, p->v[0].r, p->v[0].g, p->v[0].b, p->v[0].s, p->v[0].t);
    }
}

void gpu3d_swap(void) {
    if (!swap_pending) return;
    swap_pending = 0;
    log_polys();
    uint32_t cc = io32(0x350);
    uint32_t clear = (cc & 0x7fff) | ((cc >> 16) & 31) << 16;
    uint32_t cd = (io16(0x354) & 0x7fff) * 0x200 + 0x1ff;
    uint8_t clear_id = (uint8_t)((cc >> 24) & 63);
    for (int i = 0; i < GPU3D_W * 192; i++) {
        fb[i] = clear; depth[i] = cd; opaque_id[i] = clear_id; trans_id[i] = 0xff;
        fog_flag[i] = (uint8_t)((cc >> 15) & 1); stencil[i] = 0;
    }
    static int order[MAX_POLYS];
    int nt = 0;
    for (int i = 0; i < npolys; i++) if (!polys[i].translucent) draw_poly(&polys[i]);
    for (int i = 0; i < npolys; i++) {
        if (!polys[i].translucent) continue;
        SV sv[10];
        project(&polys[i], sv);
        float ymax = sv[0].y;
        for (int k = 1; k < polys[i].n; k++) if (sv[k].y > ymax) ymax = sv[k].y;
        sort_key[i] = ymax;
        order[nt++] = i;
    }
    if (!sort_manual) qsort(order, (size_t)nt, sizeof order[0], cmp_translucent);
    for (int k = 0; k < nt; k++) draw_poly(&polys[order[k]]);
    finish_frame(clear_id);
    npolys = 0;
}

const uint32_t *gpu3d_framebuffer(void) { return fb; }
