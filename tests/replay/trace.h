// Reader for the D3D9 call trace written by tools/d3d9_proxy (docs/TRACE_FORMAT.md, v1 and v2).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace bo1::trace {

// Interface ids and slot counts come from the proxy's method table: one source of truth.
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

class Reader {
public:
    explicit Reader(const std::string &path); // throws std::runtime_error
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    // Reads the next record; false at end of file or at a truncated tail (see truncated()).
    bool next(Record &r);
    bool truncated() const { return truncated_; }
    uint64_t size() const { return size_; }
    // 1: `depth` only counts calls recorded on return (nested calls inside other calls can appear
    //    as depth 0). 2: exact nesting; shaders and vertex declarations report their destruction.
    uint32_t version() const { return version_; }

private:
    const uint8_t *base_ = nullptr;
    uint64_t size_ = 0, pos_ = 0;
    uint32_t iface_count_ = 0, version_ = 0;
    bool truncated_ = false;
};

} // namespace bo1::trace
