// One Direct3D 9 call as the renderer sees it (ADR-004: the renderer interface is D3D9-shaped).
// The same record came from two producers: the trace reader (removed 2026-10-12, docs/TRACE_FORMAT.md) and,
// in the native runtime, the game's d3d9 shim. Arguments are the call's stack words; data the call
// carries (locked ranges, shader bytecode, vertex declarations, present parameters) travels as blobs.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace bo1::trace {

// Interface ids and slot counts come from the method table: one source of truth.
#include "d3d9_methods.h"

enum BlobTag : uint32_t {
    kOut = 1, kData = 2, kStruct = 3, kRectA = 4, kRectB = 5, kBytecode = 6, kElements = 7, kPP = 8,
    kLock = 9, kDataB = 10,
};

struct LockDesc { // payload of a kLock blob
    uint32_t format, width, height, depth, pitch, slice_pitch, lock_flags;
    uint32_t has_rect, left, top, right, bottom, front, back;
    uint32_t offset, size, data_ptr, dumped;
};

struct Blob {
    uint32_t tag;
    std::span<const uint8_t> data;
};

struct Record {
    uint8_t iface, flags, depth, nblobs;
    uint16_t slot, nwords;
    uint32_t tid, result;
    const uint32_t *words; // words[0] = this; guest pointers are object ids only
    Blob blobs[6];
    uint64_t offset;       // file offset of the record header

    bool has_result() const { return flags & 1; }
    uint32_t arg(unsigned i) const { return i < nwords ? words[i] : 0; }
    const Blob *blob(uint32_t tag) const {
        for (unsigned i = 0; i < nblobs; i++)
            if (blobs[i].tag == tag) return &blobs[i];
        return nullptr;
    }
};

const char *iface_name(unsigned iface);
const char *method_name(unsigned iface, unsigned slot);

} // namespace bo1::trace
