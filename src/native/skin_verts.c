// CPU skinning of model vertices: the four loops the worker threads spend their time in (P6 / T-064).
// docs/RE_NOTES.md "CPU skinning of model vertices" has the data layout and how it was read.
//
// The originals are SSE + MMX loops, one per number of bones a vertex is weighted to (1 to 4). The arithmetic
// below is the same operations in the same association, so results are bit-identical for finite input
// (tests/native/test_native.c compares every output byte with the lifted originals). Two things differ on
// purpose and are stated here instead of hidden:
//   - NaN input: the sign / payload of a NaN result is arm64's, not x86's (the lifted code patches it up).
//   - registers at return: eax / ecx / edx, xmm0-7 and mm0-4 are left alone. The only caller, 0x00744790,
//     reloads eax and uses nothing else of them after each call (CONFIRMED in its disassembly), and the
//     skinning job 0x00745280 issues `emms` itself.
// A rounding mode other than round-to-nearest in MXCSR would change the float -> byte packing; the game never
// sets one (docs/LIFTER.md §5), and if it is set the lifted original runs.
#include <arm_neon.h>

#include "native.h"

// constants of the image (.rdata), read from guest memory like the originals do
#define VA_W_MASK 0x00B5FF30u   // (~0, ~0, ~0, 0x80000000): keeps z and the sign of w
#define VA_W_ONE 0x00B5FF40u    // (0, 0, 0, 1.0)
#define VA_ONE 0x00B45570u      // (1, 1, 1, 1)
#define VA_PACK_BIAS 0x00B45580u  // (127, 127, 127, -192)
#define VA_PACK_SCALE 0x00B45590u // (127, 127, 127, 255)
#define VA_WEIGHT_SCALE 0x00B455A0u // 1 / 65536 in every lane

typedef struct Consts {
    uint32x4_t v_mask, v_or;
    float32x4_t bias, scale, one, wscale;
} Consts;

typedef struct Bone {
    float32x4_t r0, r1, r2, r3;   // the four 16-byte rows of a 64-byte bone matrix
} Bone;

static inline float32x4_t ldf(uint32_t a) { return vld1q_f32((const float *)(g_mem + a)); }
static inline uint16_t ld16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }

static inline Consts load_consts(void) {
    Consts c;
    uint32x4_t m = vreinterpretq_u32_f32(ldf(VA_W_MASK)), o = vreinterpretq_u32_f32(ldf(VA_W_ONE));
    // the original builds (z, z, w, w) & mask | one and takes lanes 0 and 3 of it for z and w
    c.v_mask = vsetq_lane_u32(vgetq_lane_u32(m, 0), vdupq_n_u32(0xFFFFFFFFu), 2);
    c.v_mask = vsetq_lane_u32(vgetq_lane_u32(m, 3), c.v_mask, 3);
    c.v_or = vsetq_lane_u32(vgetq_lane_u32(o, 0), vdupq_n_u32(0), 2);
    c.v_or = vsetq_lane_u32(vgetq_lane_u32(o, 3), c.v_or, 3);
    c.bias = ldf(VA_PACK_BIAS);
    c.scale = ldf(VA_PACK_SCALE);
    c.one = ldf(VA_ONE);
    c.wscale = ldf(VA_WEIGHT_SCALE);
    return c;
}

static inline Bone load_bone(uint32_t mats, uint32_t blend_entry) {
    uint32_t a = mats + ld16(blend_entry);
    Bone b = {ldf(a), ldf(a + 0x10), ldf(a + 0x20), ldf(a + 0x30)};
    return b;
}

// lanes 0-2 of r, lane 3 of q  (the original's unpckhps + shufps 0xC4 pair)
static inline float32x4_t xyz_w(float32x4_t r, float32x4_t q) {
    return vbslq_f32((uint32x4_t){0, 0, 0, 0xFFFFFFFFu}, q, r);
}

// (x * r0 + y * r1) + (z * r2 + r3)
static inline float32x4_t transform_point(float32x4_t v, const Bone *b) {
    float32x4_t x = vdupq_laneq_f32(v, 0), y = vdupq_laneq_f32(v, 1), z = vdupq_laneq_f32(v, 2);
    return vaddq_f32(vaddq_f32(vmulq_f32(x, b->r0), vmulq_f32(y, b->r1)), vaddq_f32(vmulq_f32(z, b->r2), b->r3));
}

// A packed unit vector (4 bytes: x, y, z biased by 127, and a length scale) rotated by the bone and packed again.
static inline float32x4_t rotate_packed(uint32_t packed, const Bone *b, const Consts *c) {
    uint16x8_t w = vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(packed)));
    float32x4_t n = vcvtq_f32_u32(vmovl_u16(vget_low_u16(w)));
    n = vdivq_f32(vsubq_f32(n, c->bias), c->scale);
    float32x4_t t = vmulq_f32(vdupq_laneq_f32(n, 3), n);
    float32x4_t x = vdupq_laneq_f32(t, 0), y = vdupq_laneq_f32(t, 1), z = vdupq_laneq_f32(t, 2);
    float32x4_t r = vaddq_f32(vaddq_f32(vmulq_f32(x, b->r0), vmulq_f32(y, b->r1)), vmulq_f32(z, b->r2));
    return vaddq_f32(vmulq_f32(xyz_w(r, b->r3), c->scale), c->bias);
}

// cvtps2pi with round-to-nearest: out of range and NaN give 0x80000000
static inline int32x4_t to_int_x86(float32x4_t v) {
    uint32x4_t in_range = vandq_u32(vcgeq_f32(v, vdupq_n_f32(-2147483648.0f)), vcltq_f32(v, vdupq_n_f32(2147483648.0f)));
    return vbslq_s32(in_range, vcvtnq_s32_f32(v), vdupq_n_s32(INT32_MIN));
}

// The original's three packuswb: each vector's four dwords, taken as eight signed words, saturate to eight
// bytes; those sixteen bytes, taken as eight signed words again, saturate to the eight bytes that are stored.
static inline uint64_t pack_normal_tangent(float32x4_t nrm, float32x4_t tan) {
    uint8x8_t n8 = vqmovun_s16(vreinterpretq_s16_s32(to_int_x86(nrm)));
    uint8x8_t t8 = vqmovun_s16(vreinterpretq_s16_s32(to_int_x86(tan)));
    return vget_lane_u64(vreinterpret_u64_u8(vqmovun_s16(vreinterpretq_s16_u8(vcombine_u8(n8, t8)))), 0);
}

// weight (0..65535) as a fraction, in every lane
static inline float32x4_t load_weight(uint32_t a, const Consts *c) {
    return vmulq_f32(vcvtq_f32_s32(vdupq_n_s32(ld16(a))), c->wscale);
}

// Source vertex (32 bytes): float x, y, z, w | 8 bytes copied as they are | packed normal | packed tangent.
// Of w only the sign is used: the output's fourth float is +-1.0.
#define VERTEX_HEAD()                                                                               \
    const uint8_t *s = g_mem + src + (index << 5);                                                  \
    uint8_t *d = g_mem + dst + (index << 5);                                                        \
    float32x4_t v = vreinterpretq_f32_u32(                                                          \
        vorrq_u32(vandq_u32(vreinterpretq_u32_f32(vld1q_f32((const float *)s)), c.v_mask), c.v_or)); \
    uint64_t mid;                                                                                   \
    uint32_t nrm, tan;                                                                              \
    memcpy(&mid, s + 0x10, 8);                                                                      \
    memcpy(&nrm, s + 0x18, 4);                                                                      \
    memcpy(&tan, s + 0x1C, 4);                                                                      \
    Bone a = load_bone(mats, blend)

// normal and tangent follow the first bone only, in all four variants
#define VERTEX_TAIL(pos)                                                                            \
    uint64_t nt = pack_normal_tangent(rotate_packed(nrm, &a, &c), rotate_packed(tan, &a, &c));      \
    vst1q_f32((float *)d, (pos));                                                                   \
    memcpy(d + 0x10, &mid, 8);                                                                      \
    memcpy(d + 0x18, &nt, 8)

static inline int nearest_rounding(const Cpu *cpu) { return ((cpu->mxcsr >> 13) & 3) == 0; }

// VA 0x00745520 — one bone per vertex.  eax = count, edx = blend data (u16 bone offset per vertex);
// stack: src, bone matrices, dst, int *vertex index (advanced by count).
NATIVE(00745520) {
    if (!nearest_rounding(cpu)) { f_00745520_lifted(cpu); return; }
    int32_t count = (int32_t)cpu->eax;
    uint32_t blend = cpu->edx, src = ARG(0), mats = ARG(1), dst = ARG(2), pindex = ARG(3);
    uint32_t index = M32(pindex);
    if (count > 0) {
        const Consts c = load_consts();
        for (int32_t i = 0; i < count; i++, index++, blend += 2) {
            VERTEX_HEAD();
            float32x4_t pos = xyz_w(transform_point(v, &a), v);
            VERTEX_TAIL(pos);
        }
    }
    M32(pindex) = index;
    RET();
}

// VA 0x00745730 — two bones.  Registers and stack as above; blend data per vertex: u16 offset of bone A,
// u16 offset of bone B, u16 weight of bone B.
NATIVE(00745730) {
    if (!nearest_rounding(cpu)) { f_00745730_lifted(cpu); return; }
    int32_t count = (int32_t)cpu->eax;
    uint32_t blend = cpu->edx, src = ARG(0), mats = ARG(1), dst = ARG(2), pindex = ARG(3);
    uint32_t index = M32(pindex);
    if (count > 0) {
        const Consts c = load_consts();
        for (int32_t i = 0; i < count; i++, index++, blend += 6) {
            VERTEX_HEAD();
            Bone b = load_bone(mats, blend + 2);
            float32x4_t pa = xyz_w(transform_point(v, &a), v), pb = xyz_w(transform_point(v, &b), v);
            float32x4_t pos = vaddq_f32(vmulq_f32(load_weight(blend + 4, &c), vsubq_f32(pb, pa)), pa);
            VERTEX_TAIL(pos);
        }
    }
    M32(pindex) = index;
    RET();
}

// VA 0x00745A00 — three bones.  eax = dst, edi = bone matrices; stack: src, blend data, count, int *index.
// Blend data per vertex: offset A, offset B, weight B, offset C, weight C (u16 each).
NATIVE(00745a00) {
    if (!nearest_rounding(cpu)) { f_00745a00_lifted(cpu); return; }
    uint32_t dst = cpu->eax, mats = cpu->edi, src = ARG(0), blend = ARG(1), pindex = ARG(3);
    int32_t count = (int32_t)ARG(2);
    uint32_t index = M32(pindex);
    if (count > 0) {
        const Consts c = load_consts();
        for (int32_t i = 0; i < count; i++, index++, blend += 10) {
            VERTEX_HEAD();
            Bone b = load_bone(mats, blend + 2), cc = load_bone(mats, blend + 6);
            float32x4_t pa = xyz_w(transform_point(v, &a), v), pb = xyz_w(transform_point(v, &b), v),
                        pc = xyz_w(transform_point(v, &cc), v);
            float32x4_t wb = load_weight(blend + 4, &c), wc = load_weight(blend + 8, &c);
            float32x4_t wa = vsubq_f32(vsubq_f32(c.one, wb), wc);
            float32x4_t pos = vaddq_f32(vaddq_f32(vmulq_f32(pc, wc), vmulq_f32(pb, wb)), vmulq_f32(pa, wa));
            VERTEX_TAIL(pos);
        }
    }
    M32(pindex) = index;
    RET();
}

// VA 0x00745D70 — four bones.  eax = dst, esi = bone matrices; stack as for three bones.
// Blend data per vertex: offset A, then (offset, weight) for B, C and D (u16 each).
NATIVE(00745d70) {
    if (!nearest_rounding(cpu)) { f_00745d70_lifted(cpu); return; }
    uint32_t dst = cpu->eax, mats = cpu->esi, src = ARG(0), blend = ARG(1), pindex = ARG(3);
    int32_t count = (int32_t)ARG(2);
    uint32_t index = M32(pindex);
    if (count > 0) {
        const Consts c = load_consts();
        for (int32_t i = 0; i < count; i++, index++, blend += 14) {
            VERTEX_HEAD();
            Bone b = load_bone(mats, blend + 2), cc = load_bone(mats, blend + 6), dd = load_bone(mats, blend + 10);
            float32x4_t pa = xyz_w(transform_point(v, &a), v), pb = xyz_w(transform_point(v, &b), v),
                        pc = xyz_w(transform_point(v, &cc), v), pd = xyz_w(transform_point(v, &dd), v);
            float32x4_t wb = load_weight(blend + 4, &c), wc = load_weight(blend + 8, &c), wd = load_weight(blend + 12, &c);
            float32x4_t wa = vsubq_f32(vsubq_f32(vsubq_f32(c.one, wb), wc), wd);
            float32x4_t pos = vaddq_f32(vaddq_f32(vmulq_f32(pd, wd), vaddq_f32(vmulq_f32(pc, wc), vmulq_f32(pb, wb))),
                                        vmulq_f32(pa, wa));
            VERTEX_TAIL(pos);
        }
    }
    M32(pindex) = index;
    RET();
}
