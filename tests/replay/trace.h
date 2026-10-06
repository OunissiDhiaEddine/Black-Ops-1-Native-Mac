// Reader for the D3D9 call trace written by tools/d3d9_proxy (docs/TRACE_FORMAT.md, v1 and v2).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "d3d9_call.h"

namespace bo1::trace {

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
