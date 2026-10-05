#include "trace.h"

#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bo1::trace {

namespace {
struct IfaceInfo {
    const char *com;
    unsigned count;
    const char *const *names;
};
#define NAME(i, name) #name,
#define GEN_NAMES(id, sym, com, METHODS, count) const char *const names_##sym[] = {METHODS(NAME)};
FOR_EACH_IFACE(GEN_NAMES)
#define GEN_INFO(id, sym, com, METHODS, count) {#com, count, names_##sym},
const IfaceInfo kIfaces[IFACE_COUNT] = {FOR_EACH_IFACE(GEN_INFO)};

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    uint32_t version, header_size, pointer_size, rec_hdr_size, iface_count, reserved[3];
};
struct RecHdr {
    uint8_t iface, flags, depth, nblobs;
    uint16_t slot, nwords;
    uint32_t tid, result;
};
#pragma pack(pop)
} // namespace

const char *iface_name(unsigned iface) { return iface < IFACE_COUNT ? kIfaces[iface].com : "?"; }
const char *method_name(unsigned iface, unsigned slot) {
    return iface < IFACE_COUNT && slot < kIfaces[iface].count ? kIfaces[iface].names[slot] : "?";
}

Reader::Reader(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open " + path);
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(FileHeader)) {
        ::close(fd);
        throw std::runtime_error("not a trace: " + path);
    }
    size_ = (uint64_t)st.st_size;
    void *p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) throw std::runtime_error("mmap failed: " + path);
    base_ = static_cast<const uint8_t *>(p);
    FileHeader h;
    std::memcpy(&h, base_, sizeof(h));
    if (std::memcmp(h.magic, "BO1D3D9T", 8) != 0 || (h.version != 1 && h.version != 2) || h.rec_hdr_size != sizeof(RecHdr) ||
        h.pointer_size != 4 || h.iface_count > IFACE_COUNT)
        throw std::runtime_error("unsupported trace header: " + path);
    iface_count_ = h.iface_count;
    version_ = h.version;
    pos_ = h.header_size;
}

Reader::~Reader() {
    if (base_) ::munmap(const_cast<uint8_t *>(base_), size_);
}

bool Reader::next(Record &r) {
    if (pos_ + sizeof(RecHdr) > size_) {
        truncated_ = pos_ != size_;
        return false;
    }
    RecHdr h;
    std::memcpy(&h, base_ + pos_, sizeof(h));
    uint64_t p = pos_ + sizeof(h);
    if (h.iface >= iface_count_ || h.slot >= kIfaces[h.iface].count || h.nwords > 32 || h.nblobs > 6 ||
        p + 4ull * h.nwords > size_) {
        truncated_ = true; // killed mid-record or corrupt: stop here
        return false;
    }
    r.iface = h.iface, r.flags = h.flags, r.depth = h.depth, r.nblobs = h.nblobs;
    r.slot = h.slot, r.nwords = h.nwords, r.tid = h.tid, r.result = h.result;
    r.offset = pos_;
    r.words = reinterpret_cast<const uint32_t *>(base_ + p); // file is 4-byte aligned by construction
    p += 4ull * h.nwords;
    for (unsigned i = 0; i < h.nblobs; i++) {
        if (p + 8 > size_) {
            truncated_ = true;
            return false;
        }
        uint32_t tag, len;
        std::memcpy(&tag, base_ + p, 4);
        std::memcpy(&len, base_ + p + 4, 4);
        p += 8;
        if (p + len > size_) {
            truncated_ = true;
            return false;
        }
        r.blobs[i] = {tag, {base_ + p, len}};
        p += (len + 3ull) & ~3ull;
    }
    pos_ = p;
    return true;
}

} // namespace bo1::trace
