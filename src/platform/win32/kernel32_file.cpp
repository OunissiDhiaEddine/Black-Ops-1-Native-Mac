// kernel32: files, directories, path mapping, handles (T-045).
// The guest's C:\bo1 is the working copy of the game (BO1_GAME_DIR, default artifacts/game — never
// reference/), C:\users\player is a scratch home under artifacts/boot/home.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <vector>

#include "win32_util.h"

namespace bo1 {

static std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string game_dir() {
    const char *d = getenv("BO1_GAME_DIR");
    return d ? d : "artifacts/game";
}
static std::string home_dir() {
    const char *d = getenv("BO1_HOME_DIR");
    return d ? d : "artifacts/boot/home";
}

// Absolute, normalised Windows path (backslashes, no "." / ".." components).
std::string guest_full_path(const std::string &in) {
    std::string p = in;
    for (char &c : p)
        if (c == '/') c = '\\';
    if (!(p.size() >= 2 && p[1] == ':')) p = (p.size() && p[0] == '\\') ? "C:" + p : std::string(kGuestGameDir) + "\\" + p;
    std::vector<std::string> parts;
    size_t i = 3;
    while (i <= p.size()) {
        size_t j = p.find('\\', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".")
            parts.push_back(part);
        i = j + 1;
    }
    std::string out = p.substr(0, 2);
    out[0] = (char)toupper((unsigned char)out[0]);
    for (auto &s : parts) out += "\\" + s;
    if (parts.empty()) out += "\\";
    return out;
}

std::string host_path(const std::string &win) {
    std::string full = guest_full_path(win), l = lower(full);
    std::string root, rest;
    std::string g = lower(kGuestGameDir), h = lower(kGuestHomeDir);
    if (l.compare(0, g.size(), g) == 0 && (l.size() == g.size() || l[g.size()] == '\\')) root = game_dir(), rest = full.substr(g.size());
    else if (l.compare(0, h.size(), h) == 0 && (l.size() == h.size() || l[h.size()] == '\\')) root = home_dir(), rest = full.substr(h.size());
    else root = home_dir() + "/drive_" + std::string(1, (char)tolower((unsigned char)full[0])), rest = full.substr(2);
    for (char &c : rest)
        if (c == '\\') c = '/';
    return root + rest;
}

static uint32_t errno_to_win32() {
    switch (errno) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    default: return ERROR_ACCESS_DENIED;
    }
}

struct FileObject : HObject {
    int fd;
    std::string path;
    bool is_std;
    FileObject(int f, std::string p, bool s = false) : fd(f), path(std::move(p)), is_std(s) {}
    ~FileObject() override {
        if (!is_std && fd >= 0) close(fd);
    }
    const char *kind() const override { return "file"; }
};

static void mkdirs(const std::string &path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

SHIM(kernel32, CreateFileA, 7) {
    // (lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
    std::string name = gstr(a[0]), host = host_path(name);
    bool rd = a[1] & 0x80000000u, wrt = a[1] & 0x40000000u;
    int flags = wrt ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (a[4]) {
    case 1: flags |= O_CREAT | O_EXCL; break;  // CREATE_NEW
    case 2: flags |= O_CREAT | O_TRUNC; break; // CREATE_ALWAYS
    case 3: break;                             // OPEN_EXISTING
    case 4: flags |= O_CREAT; break;           // OPEN_ALWAYS
    case 5: flags |= O_TRUNC; break;           // TRUNCATE_EXISTING
    default:
        last_error(cpu) = ERROR_INVALID_PARAMETER;
        return INVALID_HANDLE;
    }
    if (flags & O_CREAT) mkdirs(host);
    int fd = open(host.c_str(), flags, 0644);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        fd = -1;
        errno = EACCES;
    }
    if (fd < 0) {
        last_error(cpu) = errno_to_win32();
        if (g_verbose) log("CreateFileA(\"%s\") -> error %u", name.c_str(), last_error(cpu));
        return INVALID_HANDLE;
    }
    last_error(cpu) = 0;
    if (g_verbose) log("CreateFileA(\"%s\"%s) -> ok", name.c_str(), wrt ? ", write" : "");
    return handle_new(std::make_shared<FileObject>(fd, host));
}

static uint32_t g_std[3];
SHIM(kernel32, GetStdHandle, 1) {
    int idx = a[0] == 0xFFFFFFF6u ? 0 : a[0] == 0xFFFFFFF5u ? 1 : a[0] == 0xFFFFFFF4u ? 2 : -1; // STD_INPUT/OUTPUT/ERROR_HANDLE
    if (idx < 0) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return INVALID_HANDLE;
    }
    if (!g_std[idx]) g_std[idx] = handle_new(std::make_shared<FileObject>(idx, "<std>", true));
    return g_std[idx];
}
SHIM(kernel32, GetFileType, 1) {
    auto f = handle_as<FileObject>(a[0]);
    if (!f) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0; // FILE_TYPE_UNKNOWN
    }
    return f->is_std ? 2 : 1; // FILE_TYPE_CHAR : FILE_TYPE_DISK
}
SHIM(kernel32, CloseHandle, 1) {
    if (!handle_close(a[0])) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    return 1;
}
SHIM(kernel32, DuplicateHandle, 7) {
    // (hSourceProcess, hSource, hTargetProcess, lpTarget, access, inherit, options)
    std::shared_ptr<HObject> o = handle_get(a[1]);
    if (!o) {
        // pseudo handles (current thread / process): hand out a placeholder that waits never complete on
        struct Pseudo : HObject {
            const char *kind() const override { return "pseudo"; }
        };
        o = std::make_shared<Pseudo>();
    }
    if (a[3]) wr32(a[3], handle_new(o));
    return 1;
}

SHIM(kernel32, ReadFile, 5) {
    // (hFile, lpBuffer, nBytes, lpBytesRead, lpOverlapped)
    auto f = handle_as<FileObject>(a[0]);
    if (!f) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    ssize_t n;
    if (a[4]) {
        uint64_t off = rd32(a[4] + 8) | ((uint64_t)rd32(a[4] + 12) << 32);
        n = pread(f->fd, g_mem + a[1], a[2], (off_t)off);
    } else
        n = read(f->fd, g_mem + a[1], a[2]);
    if (n < 0) {
        last_error(cpu) = errno_to_win32();
        if (a[3]) wr32(a[3], 0);
        return 0;
    }
    if (a[3]) wr32(a[3], (uint32_t)n);
    if (a[4]) {
        wr32(a[4], 0);                // Internal = STATUS_SUCCESS
        wr32(a[4] + 4, (uint32_t)n);  // InternalHigh = bytes transferred
        if (n == 0 && a[2]) {
            last_error(cpu) = 38; // ERROR_HANDLE_EOF
            return 0;
        }
    }
    return 1;
}
SHIM(kernel32, ReadFileEx, 5) {
    // (hFile, lpBuffer, nBytes, lpOverlapped, lpCompletionRoutine): performed synchronously; the
    // completion routine is delivered as an APC at the next alertable wait, as on Windows.
    auto f = handle_as<FileObject>(a[0]);
    if (!f || !a[3]) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    uint64_t off = rd32(a[3] + 8) | ((uint64_t)rd32(a[3] + 12) << 32);
    ssize_t n = pread(f->fd, g_mem + a[1], a[2], (off_t)off);
    uint32_t err = n < 0 ? errno_to_win32() : (n == 0 && a[2]) ? 38u : 0u;
    wr32(a[3], err ? 0xC0000011u : 0);
    wr32(a[3] + 4, n > 0 ? (uint32_t)n : 0);
    if (a[4]) queue_apc(a[4], err, n > 0 ? (uint32_t)n : 0, a[3]);
    last_error(cpu) = 0;
    return 1;
}
SHIM(kernel32, WriteFile, 5) {
    auto f = handle_as<FileObject>(a[0]);
    if (!f) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    ssize_t n = write(f->fd, g_mem + a[1], a[2]);
    if (a[3]) wr32(a[3], n > 0 ? (uint32_t)n : 0);
    if (n < 0) {
        last_error(cpu) = errno_to_win32();
        return 0;
    }
    return 1;
}
SHIM(kernel32, FlushFileBuffers, 1) { return 1; }
SHIM(kernel32, SetFilePointer, 4) {
    // (hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod)
    auto f = handle_as<FileObject>(a[0]);
    if (!f) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0xFFFFFFFFu;
    }
    int64_t dist = a[2] ? (int64_t)(((uint64_t)rd32(a[2]) << 32) | a[1]) : (int64_t)(int32_t)a[1];
    off_t r = lseek(f->fd, (off_t)dist, a[3] == 0 ? SEEK_SET : a[3] == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) {
        last_error(cpu) = 131; // ERROR_NEGATIVE_SEEK
        return 0xFFFFFFFFu;
    }
    if (a[2]) wr32(a[2], (uint32_t)((uint64_t)r >> 32));
    last_error(cpu) = 0; // callers distinguish a low dword of 0xFFFFFFFF from failure through GetLastError
    return (uint32_t)r;
}
SHIM(kernel32, SetEndOfFile, 1) {
    auto f = handle_as<FileObject>(a[0]);
    if (!f) return 0;
    return ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0;
}
SHIM(kernel32, GetFileSize, 2) {
    auto f = handle_as<FileObject>(a[0]);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0xFFFFFFFFu;
    }
    if (a[1]) wr32(a[1], (uint32_t)((uint64_t)st.st_size >> 32));
    last_error(cpu) = 0;
    return (uint32_t)st.st_size;
}

static uint32_t attrs_of(const struct stat &st) {
    uint32_t at = S_ISDIR(st.st_mode) ? 0x10u : 0x20u; // DIRECTORY : ARCHIVE
    if (!(st.st_mode & S_IWUSR)) at |= 1;              // READONLY
    return at;
}
SHIM(kernel32, GetFileAttributesA, 1) {
    struct stat st;
    if (stat(host_path(gstr(a[0])).c_str(), &st) != 0) {
        last_error(cpu) = errno_to_win32();
        return 0xFFFFFFFFu;
    }
    return attrs_of(st);
}
SHIM(kernel32, SetFileAttributesA, 2) { return 1; }
SHIM(kernel32, CreateDirectoryA, 2) {
    std::string host = host_path(gstr(a[0]));
    mkdirs(host);
    if (mkdir(host.c_str(), 0755) != 0) {
        last_error(cpu) = errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win32();
        return 0;
    }
    return 1;
}
SHIM(kernel32, DeleteFileA, 1) {
    if (unlink(host_path(gstr(a[0])).c_str()) != 0) {
        last_error(cpu) = errno_to_win32();
        return 0;
    }
    return 1;
}
SHIM(kernel32, MoveFileA, 2) {
    std::string to = host_path(gstr(a[1]));
    struct stat st;
    if (stat(to.c_str(), &st) == 0) {
        last_error(cpu) = ERROR_ALREADY_EXISTS;
        return 0;
    }
    if (rename(host_path(gstr(a[0])).c_str(), to.c_str()) != 0) {
        last_error(cpu) = errno_to_win32();
        return 0;
    }
    return 1;
}
SHIM(kernel32, GetCurrentDirectoryA, 2) {
    uint32_t n = (uint32_t)strlen(kGuestGameDir);
    if (a[0] <= n) return n + 1;
    put_str(a[1], a[0], kGuestGameDir);
    return n;
}
SHIM(kernel32, GetFullPathNameA, 4) {
    // (lpFileName, nBufferLength, lpBuffer, lpFilePart)
    std::string full = guest_full_path(gstr(a[0]));
    if (a[1] <= full.size()) return (uint32_t)full.size() + 1;
    put_str(a[2], a[1], full);
    if (a[3]) {
        size_t p = full.find_last_of('\\');
        wr32(a[3], p + 1 < full.size() ? a[2] + (uint32_t)p + 1 : 0);
    }
    return (uint32_t)full.size();
}

// ---- FindFirstFile ---------------------------------------------------------------------------------------
static bool wild_match(const char *pat, const char *s) { // case-insensitive, '*' and '?'
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return true;
            for (; *s; s++)
                if (wild_match(pat, s)) return true;
            return false;
        }
        if (!*s || (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s))) return false;
    }
    return !*s;
}
struct FindObject : HObject {
    std::vector<std::pair<std::string, struct stat>> entries;
    size_t next = 0;
    const char *kind() const override { return "find"; }
};
static void put_find_data(uint32_t dst, const std::pair<std::string, struct stat> &e) {
    const struct stat &st = e.second;
    memset(g_mem + dst, 0, 320);
    wr32(dst, attrs_of(st));
    wr64(dst + 4, unix_to_filetime(st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec));
    wr64(dst + 12, unix_to_filetime(st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec));
    wr64(dst + 20, unix_to_filetime(st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec));
    wr32(dst + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    wr32(dst + 32, (uint32_t)st.st_size);
    put_str(dst + 44, 260, e.first);
}
SHIM(kernel32, FindFirstFileA, 2) {
    std::string pattern = guest_full_path(gstr(a[0]));
    size_t slash = pattern.find_last_of('\\');
    std::string dir = pattern.substr(0, slash), mask = pattern.substr(slash + 1);
    if (mask == "*.*") mask = "*";
    std::string hdir = host_path(dir.size() == 2 ? dir + "\\" : dir);
    DIR *d = opendir(hdir.c_str());
    auto fo = std::make_shared<FindObject>();
    if (d) {
        while (struct dirent *de = readdir(d)) {
            if (de->d_name[0] == '.' && strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) continue; // host dotfiles
            if (!wild_match(mask.c_str(), de->d_name)) continue;
            struct stat st;
            if (stat((hdir + "/" + de->d_name).c_str(), &st) != 0) continue;
            fo->entries.emplace_back(de->d_name, st);
        }
        closedir(d);
    }
    if (g_verbose) log("FindFirstFileA(\"%s\") -> %zu entries", gstr(a[0]).c_str(), fo->entries.size());
    if (fo->entries.empty()) {
        last_error(cpu) = d ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
        return INVALID_HANDLE;
    }
    put_find_data(a[1], fo->entries[0]);
    fo->next = 1;
    return handle_new(fo);
}
SHIM(kernel32, FindNextFileA, 2) {
    auto fo = handle_as<FindObject>(a[0]);
    if (!fo) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    if (fo->next >= fo->entries.size()) {
        last_error(cpu) = ERROR_NO_MORE_FILES;
        return 0;
    }
    put_find_data(a[1], fo->entries[fo->next++]);
    return 1;
}
SHIM(kernel32, FindClose, 1) { return handle_close(a[0]) ? 1 : 0; }

} // namespace bo1
