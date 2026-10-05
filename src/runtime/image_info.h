// Facts read from the PE headers of the loaded image (T-045).
#pragma once
#include <cstddef>
#include <cstdint>

namespace bo1 {

struct ImageInfo {
    uint32_t base = 0, size = 0, entry = 0;
    uint32_t stack_reserve = 0;
    uint32_t tls_start = 0, tls_end = 0, tls_index_va = 0, tls_zero_fill = 0; // IMAGE_TLS_DIRECTORY32 (VAs)
    uint32_t text_lo = 0, text_hi = 0;                                        // first executable section
};

// Parses the headers of the PE file (not the mapped image). Returns false if it is not a PE32 image.
bool image_parse(const void *file, size_t size);
const ImageInfo &image_info();

} // namespace bo1
