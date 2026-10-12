// Scene culling helpers of the main and worker threads (P6 / T-064).
#include "native.h"

static inline float ldf32(uint32_t a) { float f; memcpy(&f, g_mem + a, 4); return f; }

// VA 0x0071FBC0 — is a box outside one of a set of planes?  eax = plane set, esi = the box's floats.
// Plane set: up to 16 entries of 20 bytes (float nx, ny, nz, dist; then three bytes: the byte offsets into
// the box of the x, y and z to test — the corner farthest along the normal), count (u32) at +0x140.
// Returns eax = 1 as soon as ((x*nx + dist) + y*ny) + z*nz <= 0 for a plane, else 0. A NaN never culls
// (the original's `comiss` + `jae`).
// Not reproduced: ecx / edx and xmm0-2 at return — both call sites (0x00721738, 0x007217D5) test eax and
// reload ecx, edx and eax before using them; esi and edi are kept as the original keeps them.
NATIVE(0071fbc0) {
    uint32_t planes = cpu->eax, box = cpu->esi;
    uint32_t count = M32(planes + 0x140);
    uint32_t result = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = planes + 20 * i;
        float x = ldf32(box + M8(p + 16)), y = ldf32(box + M8(p + 17)), z = ldf32(box + M8(p + 18));
        float d = ((x * ldf32(p) + ldf32(p + 12)) + y * ldf32(p + 4)) + z * ldf32(p + 8);
        if (0.0f >= d) {
            result = 1;
            break;
        }
    }
    cpu->eax = result;
    RET();
}
