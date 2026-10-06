// wsock32 / ws2_32 on BSD sockets (ADR-011: real sockets). Winsock SOCKETs are small integers mapped
// to host descriptors; sockaddr_in is converted (Windows: 16-bit family, BSD: length byte + family
// byte); errors are reported through WSAGetLastError with the Winsock codes.
// BO1_NO_NET=1 keeps the API alive but makes every socket unusable for traffic (bind/connect fail,
// nothing is sent), for runs that must stay offline.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <map>
#include <mutex>

#include "win32_util.h"

namespace bo1 {

static std::mutex g_mu;
static std::map<uint32_t, int> g_socks;
static uint32_t g_next_sock = 0x200;
static thread_local uint32_t t_wsa_error = 0;
static bool offline() {
    static int v = -1;
    if (v < 0) v = getenv("BO1_NO_NET") ? 1 : 0;
    return v == 1;
}
static int host_fd(uint32_t s) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_socks.find(s);
    return it == g_socks.end() ? -1 : it->second;
}
static uint32_t wsa_errno() {
    switch (errno) {
    case EWOULDBLOCK: return 10035;  // WSAEWOULDBLOCK
    case EINPROGRESS: return 10036;
    case EALREADY: return 10037;
    case ENOTSOCK: case EBADF: return 10038;
    case EMSGSIZE: return 10040;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    case EINTR: return 10004;
    case EACCES: case EPERM: return 10013;
    case EINVAL: return 10022;
    default: return 10022;
    }
}
static uint32_t fail(uint32_t code = 0) {
    t_wsa_error = code ? code : wsa_errno();
    return 0xFFFFFFFFu; // SOCKET_ERROR / INVALID_SOCKET
}
static bool to_host(uint32_t ga, uint32_t len, sockaddr_in *out) {
    if (!ga || len < 16 || rd16(ga) != 2 /* AF_INET */) return false;
    memset(out, 0, sizeof(*out));
    out->sin_len = sizeof(*out);
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, g_mem + ga + 2, 2);
    memcpy(&out->sin_addr, g_mem + ga + 4, 4);
    return true;
}
static void to_guest(const sockaddr_in &in, uint32_t ga, uint32_t len_va) {
    if (!ga) return;
    memset(g_mem + ga, 0, 16);
    wr16(ga, 2);
    memcpy(g_mem + ga + 2, &in.sin_port, 2);
    memcpy(g_mem + ga + 4, &in.sin_addr, 4);
    if (len_va) wr32(len_va, 16);
}

static uint32_t w_socket(uint32_t af, uint32_t type, uint32_t proto) {
    if (af != 2) return fail(10047); // WSAEAFNOSUPPORT
    int fd = ::socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, (int)proto);
    if (fd < 0) return fail();
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t s = g_next_sock;
    g_next_sock += 4;
    g_socks[s] = fd;
    return s;
}
static uint32_t w_closesocket(uint32_t s) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) return fail(10038);
    close(it->second);
    g_socks.erase(it);
    return 0;
}
static uint32_t w_bind(uint32_t s, uint32_t addr, uint32_t len) {
    sockaddr_in sa;
    int fd = host_fd(s);
    if (fd < 0 || !to_host(addr, len, &sa)) return fail(10038);
    if (offline()) return fail(10013);
    return ::bind(fd, (sockaddr *)&sa, sizeof(sa)) == 0 ? 0 : fail();
}
static uint32_t w_connect(uint32_t s, uint32_t addr, uint32_t len) {
    sockaddr_in sa;
    int fd = host_fd(s);
    if (fd < 0 || !to_host(addr, len, &sa)) return fail(10038);
    if (offline()) return fail(10051);
    return ::connect(fd, (sockaddr *)&sa, sizeof(sa)) == 0 ? 0 : fail();
}
static uint32_t w_send(uint32_t s, uint32_t buf, uint32_t len, uint32_t flags) {
    int fd = host_fd(s);
    if (fd < 0) return fail(10038);
    if (offline()) return len;
    ssize_t n = ::send(fd, g_mem + buf, len, 0);
    return n < 0 ? fail() : (uint32_t)n;
}
static uint32_t w_recv(uint32_t s, uint32_t buf, uint32_t len, uint32_t flags) {
    int fd = host_fd(s);
    if (fd < 0) return fail(10038);
    ssize_t n = ::recv(fd, g_mem + buf, len, (flags & 2) ? MSG_PEEK : 0);
    return n < 0 ? fail() : (uint32_t)n;
}
static uint32_t w_sendto(uint32_t s, uint32_t buf, uint32_t len, uint32_t flags, uint32_t to, uint32_t tolen) {
    sockaddr_in sa;
    int fd = host_fd(s);
    if (fd < 0 || !to_host(to, tolen, &sa)) return fail(10038);
    if (offline()) return len;
    ssize_t n = ::sendto(fd, g_mem + buf, len, 0, (sockaddr *)&sa, sizeof(sa));
    return n < 0 ? fail() : (uint32_t)n;
}
static uint32_t w_recvfrom(uint32_t s, uint32_t buf, uint32_t len, uint32_t flags, uint32_t from, uint32_t fromlen) {
    int fd = host_fd(s);
    if (fd < 0) return fail(10038);
    sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    ssize_t n = ::recvfrom(fd, g_mem + buf, len, (flags & 2) ? MSG_PEEK : 0, (sockaddr *)&sa, &sl);
    if (n < 0) return fail();
    to_guest(sa, from, fromlen);
    return (uint32_t)n;
}
static uint32_t w_ioctlsocket(uint32_t s, uint32_t cmd, uint32_t argp) {
    int fd = host_fd(s);
    if (fd < 0) return fail(10038);
    if (cmd == 0x8004667Eu) { // FIONBIO
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, rd32(argp) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
        return 0;
    }
    if (cmd == 0x4004667Fu) { // FIONREAD
        int n = 0;
        ioctl(fd, FIONREAD, &n);
        wr32(argp, (uint32_t)n);
        return 0;
    }
    return fail(10022);
}
static uint32_t w_setsockopt(uint32_t s, uint32_t level, uint32_t name, uint32_t val, uint32_t len) {
    int fd = host_fd(s);
    if (fd < 0) return fail(10038);
    int v = len >= 4 ? (int)rd32(val) : len ? g_mem[val] : 0;
    int hl = -1, hn = -1;
    if (level == 0xFFFF) { // SOL_SOCKET
        hl = SOL_SOCKET;
        switch (name) {
        case 0x0004: hn = SO_REUSEADDR; break;
        case 0x0020: hn = SO_BROADCAST; break;
        case 0x1001: hn = SO_SNDBUF; break;
        case 0x1002: hn = SO_RCVBUF; break;
        case 0x0008: hn = SO_KEEPALIVE; break;
        }
    } else if (level == 6 && name == 1) { // IPPROTO_TCP / TCP_NODELAY
        return 0;
    }
    if (hn < 0) {
        log("setsockopt(level %#x, option %#x) ignored", level, name);
        return 0;
    }
    return ::setsockopt(fd, hl, hn, &v, sizeof(v)) == 0 ? 0 : fail();
}
static uint32_t w_getsockname(uint32_t s, uint32_t addr, uint32_t len_va) {
    int fd = host_fd(s);
    sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    if (fd < 0 || ::getsockname(fd, (sockaddr *)&sa, &sl) != 0) return fail(10038);
    to_guest(sa, addr, len_va);
    return 0;
}
// fd_set (Winsock): u_int fd_count; SOCKET fd_array[64]
static void set_in(uint32_t ga, fd_set *out, int *maxfd) {
    FD_ZERO(out);
    if (!ga) return;
    for (uint32_t i = 0, n = rd32(ga); i < n && i < 64; i++) {
        int fd = host_fd(rd32(ga + 4 + 4 * i));
        if (fd >= 0 && fd < FD_SETSIZE) {
            FD_SET(fd, out);
            if (fd > *maxfd) *maxfd = fd;
        }
    }
}
static uint32_t set_out(uint32_t ga, const fd_set *in) {
    if (!ga) return 0;
    uint32_t n = rd32(ga), kept = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        uint32_t s = rd32(ga + 4 + 4 * i);
        int fd = host_fd(s);
        if (fd >= 0 && fd < FD_SETSIZE && FD_ISSET(fd, in)) wr32(ga + 4 + 4 * kept++, s);
    }
    wr32(ga, kept);
    return kept;
}
static uint32_t w_select(uint32_t nfds, uint32_t r, uint32_t w, uint32_t e, uint32_t timeout) {
    fd_set rs, ws, es;
    int maxfd = -1;
    set_in(r, &rs, &maxfd);
    set_in(w, &ws, &maxfd);
    set_in(e, &es, &maxfd);
    timeval tv, *ptv = nullptr;
    if (timeout) {
        tv.tv_sec = (int32_t)rd32(timeout);
        tv.tv_usec = (int32_t)rd32(timeout + 4);
        ptv = &tv;
    }
    int n = ::select(maxfd + 1, &rs, &ws, &es, ptv);
    if (n < 0) return fail();
    return set_out(r, &rs) + set_out(w, &ws) + set_out(e, &es);
}
static uint32_t w_fd_isset(uint32_t s, uint32_t set) {
    for (uint32_t i = 0, n = rd32(set); i < n && i < 64; i++)
        if (rd32(set + 4 + 4 * i) == s) return 1;
    return 0;
}
static uint32_t w_gethostname(uint32_t buf, uint32_t len) {
    put_str(buf, len, "bo1-host");
    return 0;
}
static uint32_t w_gethostbyname(uint32_t name_va) {
    // hostent: h_name, h_aliases, h_addrtype (short), h_length (short), h_addr_list — one static block per thread
    static thread_local uint32_t block = 0;
    std::string name = gstr(name_va);
    in_addr addr;
    addr.s_addr = htonl(INADDR_LOOPBACK);
    if (name != "bo1-host" && name != "localhost") {
        if (offline()) return t_wsa_error = 11001, 0; // WSAHOST_NOT_FOUND
        hostent *he = ::gethostbyname(name.c_str());
        if (!he || he->h_addrtype != AF_INET) {
            if (g_verbose) log("gethostbyname(\"%s\") -> not found", name.c_str());
            return t_wsa_error = 11001, 0;
        }
        memcpy(&addr, he->h_addr_list[0], 4);
    }
    if (g_verbose) log("gethostbyname(\"%s\") -> %s", name.c_str(), inet_ntoa(addr));
    if (!block) block = heap_alloc(16 + 8 + 8 + 4 + 256, true);
    uint32_t aliases = block + 16, list = block + 24, ip = block + 32, nm = block + 36;
    put_str(nm, 256, name);
    memcpy(g_mem + ip, &addr, 4);
    wr32(list, ip);
    wr32(list + 4, 0);
    wr32(aliases, 0);
    wr32(block, nm);
    wr32(block + 4, aliases);
    wr16(block + 8, 2);
    wr16(block + 10, 4);
    wr32(block + 12, list);
    return block;
}
static uint32_t w_inet_ntoa(uint32_t addr) {
    static thread_local uint32_t buf = 0;
    if (!buf) buf = heap_alloc(32, true);
    in_addr a;
    a.s_addr = addr;
    put_str(buf, 32, inet_ntoa(a));
    return buf;
}

// The game imports part of the API from wsock32 and part from ws2_32; both names map to the same code.
#define BOTH(name, n, call)                                  \
    SHIM(wsock32, name, n) { return call; }                 \
    SHIM(ws2_32, name, n) { return call; }
BOTH(socket, 3, w_socket(a[0], a[1], a[2]))
BOTH(closesocket, 1, w_closesocket(a[0]))
BOTH(bind, 3, w_bind(a[0], a[1], a[2]))
BOTH(connect, 3, w_connect(a[0], a[1], a[2]))
BOTH(send, 4, w_send(a[0], a[1], a[2], a[3]))
BOTH(recv, 4, w_recv(a[0], a[1], a[2], a[3]))
BOTH(sendto, 6, w_sendto(a[0], a[1], a[2], a[3], a[4], a[5]))
BOTH(recvfrom, 6, w_recvfrom(a[0], a[1], a[2], a[3], a[4], a[5]))
BOTH(ioctlsocket, 3, w_ioctlsocket(a[0], a[1], a[2]))
BOTH(setsockopt, 5, w_setsockopt(a[0], a[1], a[2], a[3], a[4]))
BOTH(getsockname, 3, w_getsockname(a[0], a[1], a[2]))
BOTH(select, 5, w_select(a[0], a[1], a[2], a[3], a[4]))
BOTH(__WSAFDIsSet, 2, w_fd_isset(a[0], a[1]))
BOTH(gethostname, 2, w_gethostname(a[0], a[1]))
BOTH(gethostbyname, 1, w_gethostbyname(a[0]))
BOTH(inet_ntoa, 1, w_inet_ntoa(a[0]))
BOTH(inet_addr, 1, (uint32_t)inet_addr(gstr(a[0]).c_str()))
BOTH(htons, 1, (uint32_t)htons((uint16_t)a[0]))
BOTH(ntohs, 1, (uint32_t)ntohs((uint16_t)a[0]))
BOTH(WSAGetLastError, 0, t_wsa_error)
BOTH(WSACleanup, 0, 0)

} // namespace bo1
