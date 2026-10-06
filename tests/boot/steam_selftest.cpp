// Self-test of the Steam stand-in (src/platform/win32/steam_local.cpp):  artifacts/boot/bo1_boot --steam-selftest
// Runs on the guest main thread with the game image loaded but not started. It calls the stand-in the
// way the game does — the flat imports through their trampolines, the interface methods through the
// guest-visible vtables with `this` in ecx — and checks for every method in re/steam_abi.csv that the
// stack comes back balanced (callee pops exactly its arguments), plus the answers the game depends on.
// The Steam id is checked with the game's own CSteamID::IsValid (VA 0x00573570), and call-result
// delivery with a stand-in for the game's CCallResult object.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/platform/win32/win32_util.h"

namespace bo1 {
namespace {

int g_failed = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}
#define CHECK(cond) check((cond), #cond)

struct Abi {
    const char *iface;
    int slot;
    const char *method;
    int words;
};
const Abi kAbi[] = {
#include "steam_abi.inc"
};

uint32_t flat(Cpu *cpu, const char *name, std::initializer_list<uint32_t> args = {}) {
    uint32_t t = import_lookup("steam_api.dll", name);
    if (!t) {
        fprintf(stderr, "FAIL: no shim for %s\n", name);
        g_failed++;
        return 0;
    }
    return guest_call(cpu, t, args);
}

// thiscall through the object's vtable; reports whether the callee popped exactly its arguments.
uint32_t method(Cpu *cpu, uint32_t obj, int slot, const std::vector<uint32_t> &args, bool *balanced = nullptr) {
    uint32_t before = cpu->esp;
    for (auto it = args.rbegin(); it != args.rend(); ++it) PUSH32(*it);
    PUSH32(0xFFEF0000u); // return address (never executed)
    cpu->ecx = obj;
    rt_dispatch(cpu, rd32(rd32(obj) + 4u * (uint32_t)slot));
    if (balanced) *balanced = cpu->esp == before;
    cpu->esp = before;
    return cpu->eax;
}
const Abi *find(const char *iface, const char *name) {
    for (const Abi &a : kAbi)
        if (!strcmp(a.iface, iface) && !strcmp(a.method, name)) return &a;
    fprintf(stderr, "FAIL: %s::%s is not in re/steam_abi.csv\n", iface, name);
    g_failed++;
    return &kAbi[0];
}

// Stand-in for the game's CCallResult<…>::Run(void *param, bool ioFailure, SteamAPICall_t call) at vtable[0].
struct Seen {
    int calls = 0;
    uint32_t self = 0, value = 0, io_failure = 0;
    uint64_t call = 0;
} g_seen;
uint32_t fake_run(Cpu *, const uint32_t *a) {
    g_seen.calls++;
    g_seen.self = a[0];
    g_seen.value = rd32(a[1]);
    g_seen.io_failure = a[2];
    g_seen.call = (uint64_t)a[3] | ((uint64_t)a[4] << 32);
    return 0;
}

} // namespace

int steam_selftest(Cpu *cpu) {
    uint32_t scratch = heap_alloc(4096, true);

    CHECK(flat(cpu, "SteamAPI_RestartAppIfNecessary", {42700}) == 0);
    CHECK((flat(cpu, "SteamAPI_Init") & 0xFF) == 1);
    CHECK(flat(cpu, "SteamGameServer_Init", {0, 0, 0, 0, 0, 0, 0, 0}) == 0);
    CHECK(flat(cpu, "SteamGameServer") == 0);
    CHECK(flat(cpu, "SteamMasterServerUpdater") == 0);

    // every listed method: callable through its vtable slot, stack balanced
    for (const Abi &a : kAbi) {
        uint32_t obj = flat(cpu, a.iface);
        if (!obj) {
            fprintf(stderr, "FAIL: %s() returned NULL\n", a.iface);
            g_failed++;
            continue;
        }
        std::vector<uint32_t> args((size_t)a.words - 1, scratch); // every argument a valid, zeroed guest buffer
        bool balanced = false;
        method(cpu, obj, a.slot, args, &balanced);
        g_checks++;
        if (!balanced) {
            g_failed++;
            fprintf(stderr, "FAIL: %s::%s (slot %d) left the stack unbalanced\n", a.iface, a.method, a.slot);
        }
        CHECK(flat(cpu, a.iface) == obj); // accessors are singletons
    }

    // the answers the game's start-up depends on
    uint32_t user = flat(cpu, "SteamUser"), utils = flat(cpu, "SteamUtils"), apps = flat(cpu, "SteamApps");
    uint32_t friends = flat(cpu, "SteamFriends"), stats = flat(cpu, "SteamUserStats"), net = flat(cpu, "SteamNetworking");
    CHECK(method(cpu, utils, find("SteamUtils", "GetAppID")->slot, {}) == 42700);
    uint32_t id = scratch + 64;
    CHECK(method(cpu, user, find("SteamUser", "GetSteamID")->slot, {id}) == id);
    CHECK(rd32(id) != 0 || rd32(id + 4) != 0);
    cpu->ecx = id; // the game's own validity test, as at VA 0x005A2769
    PUSH32(0xFFEF0000u);
    uint32_t sp = cpu->esp + 4;
    rt_dispatch(cpu, 0x00573570);
    cpu->esp = sp;
    check((cpu->eax & 0xFF) != 0, "game's CSteamID::IsValid (0x00573570) accepts the local user id");
    CHECK((method(cpu, user, find("SteamUser", "BLoggedOn")->slot, {}) & 0xFF) == 0);
    CHECK((method(cpu, apps, find("SteamApps", "BIsDlcInstalled")->slot, {42716}) & 0xFF) == 0);
    CHECK(method(cpu, friends, find("SteamFriends", "GetFriendCount")->slot, {4}) == 0);
    uint32_t name = method(cpu, friends, find("SteamFriends", "GetPersonaName")->slot, {});
    CHECK(name != 0 && strlen((const char *)(g_mem + name)) > 0);
    CHECK((method(cpu, stats, find("SteamUserStats", "StoreStats")->slot, {}) & 0xFF) == 1);
    CHECK((method(cpu, stats, find("SteamUserStats", "RequestCurrentStats")->slot, {}) & 0xFF) == 0);
    wr32(scratch + 128, 77);
    CHECK((method(cpu, net, find("SteamNetworking", "IsP2PPacketAvailable")->slot, {scratch + 128, 0}) & 0xFF) == 0);
    CHECK(rd32(scratch + 128) == 0);

    // callbacks: registration marks the object like Valve's library does
    uint32_t cb = scratch + 256;
    flat(cpu, "SteamAPI_RegisterCallback", {cb, 1101});
    CHECK((g_mem[cb + 4] & 1) == 1 && rd32(cb + 8) == 1101);
    flat(cpu, "SteamAPI_UnregisterCallback", {cb});
    CHECK((g_mem[cb + 4] & 1) == 0);

    // call results: the encrypted app ticket request completes with "no connection" on the next pump
    uint32_t result_obj = scratch + 512, vtable = scratch + 768;
    wr32(vtable, tramp_register_thiscall("test", "CCallResult::Run", fake_run, 5));
    wr32(result_obj, vtable);
    int slot = find("SteamUser", "RequestEncryptedAppTicket")->slot;
    uint64_t call = method(cpu, user, slot, {0, 0});
    call |= (uint64_t)cpu->edx << 32;
    CHECK(call != 0);
    flat(cpu, "SteamAPI_RegisterCallResult", {result_obj, (uint32_t)call, (uint32_t)(call >> 32)});
    CHECK(g_seen.calls == 0);
    flat(cpu, "SteamAPI_RunCallbacks");
    CHECK(g_seen.calls == 1 && g_seen.self == result_obj && g_seen.call == call);
    CHECK(g_seen.value == 3 /* k_EResultNoConnection */ && g_seen.io_failure == 0);
    flat(cpu, "SteamAPI_RunCallbacks");
    CHECK(g_seen.calls == 1); // fires once
    uint64_t call2 = method(cpu, user, slot, {0, 0});
    call2 |= (uint64_t)cpu->edx << 32;
    CHECK(call2 != 0 && call2 != call);
    flat(cpu, "SteamAPI_RegisterCallResult", {result_obj, (uint32_t)call2, (uint32_t)(call2 >> 32)});
    flat(cpu, "SteamAPI_UnregisterCallResult", {result_obj, (uint32_t)call2, (uint32_t)(call2 >> 32)});
    flat(cpu, "SteamAPI_RunCallbacks");
    CHECK(g_seen.calls == 1); // unregistered: not delivered
    uint32_t len = 99;
    wr32(scratch + 132, len);
    CHECK((method(cpu, user, find("SteamUser", "GetEncryptedAppTicket")->slot, {scratch + 1024, 256, scratch + 132}) & 0xFF) == 0);
    CHECK(rd32(scratch + 132) == 0);

    fprintf(stderr, "steam selftest: %d checks, %d failed, %zu interface methods\n", g_checks, g_failed, sizeof(kAbi) / sizeof(kAbi[0]));
    return g_failed ? 1 : 0;
}

} // namespace bo1
