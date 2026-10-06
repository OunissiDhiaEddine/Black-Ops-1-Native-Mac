// steam_api.dll: a local stand-in for the Steam client (ADR-015, user decision 2026-10-06).
//
// The shipped steam_api.dll is not loaded (it cannot run in this process, ADR-010 spike). The game's 19
// Steam imports are served here, and the C++ interfaces they return are host-implemented objects with
// guest-visible vtables (docs/HOST_ABI.md §4.3; MSVC thiscall, `this` in ecx).
//
// What the game is told is what a Steam client without a connection would say: Steam is there, a local
// user exists, nobody is logged on to the Steam servers, there are no friends, no stats, no DLC, no
// game server, and requests that need Valve's back end fail with "no connection". Nothing talks to
// Steam or to any network.
//
// Interface layout: only the methods the game is known to call exist (re/steam_abi.csv — slot, stack
// words and return convention of each were read off the game's own call sites, tools/steam_sites.py).
// Every other vtable slot traps with the interface name and slot number, so an unexpected call is
// reported instead of being answered with a guess.
//
//   BO1_STEAM_ABSENT=1        diagnostics: answer "Steam is not running" (SteamAPI_Init fails).
//   BO1_EMULATED_STEAM_DLL=1  load the shipped DLL in the x86 interpreter instead (tests/boot/main.cpp).
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "win32_util.h"

namespace bo1 {
namespace {

bool absent() {
    static const bool v = getenv("BO1_STEAM_ABSENT") != nullptr;
    return v;
}

constexpr uint32_t kAppId = 42700; // Call of Duty: Black Ops (the game compares callback game ids with it)
// CSteamID: account id (bits 0-31) | instance (32-51) | account type (52-55) | universe (56-63).
// A local user: universe "public" (1), type "individual" (1), instance 1 (desktop), account 1. The game
// requires CSteamID::IsValid() on it at start-up (VA 0x005A276B -> 0x00573570).
constexpr uint64_t kLocalUser = (1ull << 56) | (1ull << 52) | (1ull << 32) | 1ull;
const char *const kPersonaName = "Player";

enum : uint32_t { k_EResultFail = 2, k_EResultNoConnection = 3 };
enum : int { k_iEncryptedAppTicketResponse = 154 }; // k_iSteamUserCallbacks (100) + 54

// ---- callbacks and call results ------------------------------------------------------------------
// The game's CCallbackBase objects (CONFIRMED from its vtables at 0x009EAD38 / 0x00A200E8 and the
// CCallResult set-up at 0x0066BA18): +0 vtable, +4 flags (bit 0 = registered), +8 callback id,
// call results: +0x10 the 64-bit call handle. MSVC orders the overloads so that
//   vtable[0] = Run(void *param, bool ioFailure, SteamAPICall_t call)   (ret 0x10)
//   vtable[1] = Run(void *param)                                         (ret 4)
//   vtable[2] = GetCallbackSizeBytes()
enum : uint32_t { CB_FLAGS = 4, CB_ID = 8 };

struct Completed { // a finished asynchronous call, delivered by the next SteamAPI_RunCallbacks
    uint64_t call;
    int id;
    std::vector<uint8_t> data;
    bool io_failure;
};
std::mutex g_mu;
std::map<uint32_t, int> g_callbacks;          // CCallbackBase* -> callback id
std::map<uint64_t, uint32_t> g_call_results;  // call handle -> CCallbackBase*
std::deque<Completed> g_completed;
uint64_t g_next_call = 1;
uint32_t g_scratch = 0; // guest buffer the callback structs are handed over in

uint64_t new_call(int id, const void *data, size_t size, bool io_failure = false) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t h = g_next_call++;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    g_completed.push_back({h, id, std::vector<uint8_t>(p, p + size), io_failure});
    return h;
}

void run_callbacks(Cpu *cpu) {
    for (;;) {
        Completed c;
        uint32_t target = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_completed.empty()) return;
            c = std::move(g_completed.front());
            g_completed.pop_front();
            auto it = g_call_results.find(c.call);
            if (it != g_call_results.end()) {
                target = it->second;
                g_call_results.erase(it); // a call result fires once
            }
            if (!g_scratch) g_scratch = heap_alloc(1024, true);
        }
        if (!target || c.data.size() > 1024) continue; // nobody is waiting for it
        memcpy(g_mem + g_scratch, c.data.data(), c.data.size());
        log("steam: call result %d for call %llu -> object %08x", c.id, (unsigned long long)c.call, target);
        cpu->ecx = target;
        guest_call(cpu, rd32(rd32(target)), {g_scratch, c.io_failure ? 1u : 0u, (uint32_t)c.call, (uint32_t)(c.call >> 32)});
    }
}

uint32_t ret64(Cpu *cpu, uint64_t v) {
    cpu->edx = (uint32_t)(v >> 32);
    return (uint32_t)v;
}

// ---- interface methods. a[0] = this, a[1..] = stack arguments ---------------------------------------
using M = uint32_t(Cpu *, const uint32_t *);

// ISteamUser
M user_BLoggedOn, user_GetSteamID, user_InitiateGameConnection, user_TerminateGameConnection,
    user_RequestEncryptedAppTicket, user_GetEncryptedAppTicket;
uint32_t user_BLoggedOn(Cpu *, const uint32_t *) { return 0; } // no connection to the Steam servers
uint32_t user_GetSteamID(Cpu *, const uint32_t *a) {           // result through the hidden pointer a[1]
    wr64(a[1], kLocalUser);
    return a[1];
}
uint32_t user_InitiateGameConnection(Cpu *, const uint32_t *) { return 0; } // no auth blob
uint32_t user_TerminateGameConnection(Cpu *, const uint32_t *) { return 0; }
uint32_t user_RequestEncryptedAppTicket(Cpu *cpu, const uint32_t *) {
    // The ticket comes from Valve's servers; without them the request completes with "no connection"
    // (EncryptedAppTicketResponse_t { EResult m_eResult; }).
    uint32_t result = k_EResultNoConnection;
    uint64_t h = new_call(k_iEncryptedAppTicketResponse, &result, sizeof(result));
    log("steam: RequestEncryptedAppTicket -> call %llu (will fail: no connection)", (unsigned long long)h);
    return ret64(cpu, h);
}
uint32_t user_GetEncryptedAppTicket(Cpu *, const uint32_t *a) {
    if (a[3]) wr32(a[3], 0);
    return 0;
}

// ISteamUserStats: no stats service. Achievements are accepted and dropped.
uint32_t stats_RequestCurrentStats(Cpu *, const uint32_t *) { return 0; } // "not logged on"
uint32_t stats_GetAchievement(Cpu *, const uint32_t *a) {
    if (a[2]) g_mem[a[2]] = 0;
    return 0;
}
uint32_t stats_SetAchievement(Cpu *, const uint32_t *a) {
    log("steam: SetAchievement(\"%s\") dropped (no stats service)", a[1] ? (const char *)(g_mem + a[1]) : "");
    return 0;
}
// The game calls StoreStats every frame until it succeeds (VA 0x00403AD0); there is nothing to store.
uint32_t stats_StoreStats(Cpu *, const uint32_t *) { return 1; }

// ISteamUtils
uint32_t utils_GetAppID(Cpu *, const uint32_t *) { return kAppId; }
uint32_t utils_SetOverlayNotificationPosition(Cpu *, const uint32_t *) { return 0; } // no overlay

// ISteamApps
uint32_t apps_BIsDlcInstalled(Cpu *, const uint32_t *a) {
    log("steam: BIsDlcInstalled(%u) -> false", a[1]);
    return 0;
}

// ISteamFriends: the local user, no friends, no overlay.
uint32_t friends_GetPersonaName(Cpu *, const uint32_t *) {
    static uint32_t name = 0;
    if (!name) name = heap_strdup(kPersonaName);
    return name;
}
uint32_t friends_GetFriendCount(Cpu *, const uint32_t *) { return 0; }
uint32_t friends_GetFriendByIndex(Cpu *, const uint32_t *a) { // hidden result pointer a[1]
    wr64(a[1], 0);                                            // k_steamIDNil
    return a[1];
}
uint32_t friends_GetFriendRelationship(Cpu *, const uint32_t *) { return 0; } // k_EFriendRelationshipNone
uint32_t friends_GetFriendPersonaState(Cpu *, const uint32_t *) { return 0; } // k_EPersonaStateOffline
uint32_t friends_ActivateGameOverlayToUser(Cpu *, const uint32_t *) { return 0; }
uint32_t friends_ActivateGameOverlayToStore(Cpu *, const uint32_t *) { return 0; }

// ISteamNetworking: no peers.
uint32_t net_SendP2PPacket(Cpu *, const uint32_t *) { return 0; }
uint32_t net_IsP2PPacketAvailable(Cpu *, const uint32_t *a) {
    if (a[1]) wr32(a[1], 0);
    return 0;
}
uint32_t net_ReadP2PPacket(Cpu *, const uint32_t *a) {
    if (a[3]) wr32(a[3], 0);
    return 0;
}
uint32_t net_AcceptP2PSessionWithUser(Cpu *, const uint32_t *) { return 0; }

struct Impl {
    const char *iface, *method;
    ShimFn fn;
};
const Impl kImpl[] = {
    {"SteamUser", "BLoggedOn", user_BLoggedOn},
    {"SteamUser", "GetSteamID", user_GetSteamID},
    {"SteamUser", "InitiateGameConnection", user_InitiateGameConnection},
    {"SteamUser", "TerminateGameConnection", user_TerminateGameConnection},
    {"SteamUser", "RequestEncryptedAppTicket", user_RequestEncryptedAppTicket},
    {"SteamUser", "GetEncryptedAppTicket", user_GetEncryptedAppTicket},
    {"SteamUserStats", "RequestCurrentStats", stats_RequestCurrentStats},
    {"SteamUserStats", "GetAchievement", stats_GetAchievement},
    {"SteamUserStats", "SetAchievement", stats_SetAchievement},
    {"SteamUserStats", "StoreStats", stats_StoreStats},
    {"SteamUtils", "GetAppID", utils_GetAppID},
    {"SteamUtils", "SetOverlayNotificationPosition", utils_SetOverlayNotificationPosition},
    {"SteamApps", "BIsDlcInstalled", apps_BIsDlcInstalled},
    {"SteamFriends", "GetPersonaName", friends_GetPersonaName},
    {"SteamFriends", "GetFriendCount", friends_GetFriendCount},
    {"SteamFriends", "GetFriendByIndex", friends_GetFriendByIndex},
    {"SteamFriends", "GetFriendRelationship", friends_GetFriendRelationship},
    {"SteamFriends", "GetFriendPersonaState", friends_GetFriendPersonaState},
    {"SteamFriends", "ActivateGameOverlayToUser", friends_ActivateGameOverlayToUser},
    {"SteamFriends", "ActivateGameOverlayToStore", friends_ActivateGameOverlayToStore},
    {"SteamNetworking", "SendP2PPacket", net_SendP2PPacket},
    {"SteamNetworking", "IsP2PPacketAvailable", net_IsP2PPacketAvailable},
    {"SteamNetworking", "ReadP2PPacket", net_ReadP2PPacket},
    {"SteamNetworking", "AcceptP2PSessionWithUser", net_AcceptP2PSessionWithUser},
};

// ---- interface objects (one per interface, built on first use) ---------------------------------------
struct Abi { // re/steam_abi.csv -> gen/steam_abi.inc; `words` counts `this`
    const char *iface;
    int slot;
    const char *method;
    int words;
};
const Abi kAbi[] = {
#include "steam_abi.inc"
};
constexpr int kVtableSlots = 48; // more than any of these interface versions has

uint32_t interface_object(const char *iface) {
    if (absent()) return 0;
    static std::mutex mu;
    static std::map<std::string, uint32_t> objects;
    static std::deque<std::string> names; // ComMethod keeps the pointer; deque elements never move
    std::lock_guard<std::mutex> lk(mu);
    auto it = objects.find(iface);
    if (it != objects.end()) return it->second;
    std::vector<ComMethod> methods;
    for (int slot = 0; slot < kVtableSlots; slot++) {
        ComMethod m = {nullptr, 1, nullptr};
        for (const Abi &a : kAbi) {
            if (a.slot != slot || strcmp(a.iface, iface) != 0) continue;
            for (const Impl &i : kImpl)
                if (!strcmp(i.iface, iface) && !strcmp(i.method, a.method)) m = {a.method, a.words, i.fn};
            if (!m.fn) {
                fprintf(stderr, "[bo1] re/steam_abi.csv lists %s::%s but steam_local.cpp does not implement it\n", iface, a.method);
                abort();
            }
        }
        if (!m.fn) { // not a method the game is known to call: trap, naming the slot
            names.push_back("slot " + std::to_string(slot) + " (not implemented)");
            m.name = names.back().c_str();
        }
        methods.push_back(m);
    }
    uint32_t obj = com_new(com_vtable_thiscall(iface, methods.data(), methods.size()), 0);
    log("steam: %s interface object at %08x", iface, obj);
    return objects[iface] = obj;
}

} // namespace

// ---- the 19 flat imports (cdecl) -------------------------------------------------------------------------
SHIM(steam_api, SteamAPI_Init, -1) {
    if (absent()) {
        log("SteamAPI_Init -> false (BO1_STEAM_ABSENT)");
        return 0;
    }
    log("SteamAPI_Init -> true (local stand-in: offline, no Steam client involved)");
    return 1;
}
SHIM(steam_api, SteamAPI_RestartAppIfNecessary, -1) { return 0; } // no relaunch through a Steam client
SHIM(steam_api, SteamAPI_Shutdown, -1) { return 0; }
SHIM(steam_api, SteamAPI_RunCallbacks, -1) {
    run_callbacks(cpu);
    return 0;
}
SHIM(steam_api, SteamAPI_RegisterCallback, -1) { // (CCallbackBase *callback, int id)
    if (!a[0]) return 0;
    g_mem[a[0] + CB_FLAGS] |= 1;
    wr32(a[0] + CB_ID, a[1]);
    std::lock_guard<std::mutex> lk(g_mu);
    g_callbacks[a[0]] = (int)a[1];
    return 0;
}
SHIM(steam_api, SteamAPI_UnregisterCallback, -1) { // (CCallbackBase *callback)
    if (!a[0]) return 0;
    g_mem[a[0] + CB_FLAGS] &= (uint8_t)~1u;
    std::lock_guard<std::mutex> lk(g_mu);
    g_callbacks.erase(a[0]);
    return 0;
}
SHIM(steam_api, SteamAPI_RegisterCallResult, -1) { // (CCallbackBase *callback, SteamAPICall_t call)
    std::lock_guard<std::mutex> lk(g_mu);
    g_call_results[(uint64_t)a[1] | ((uint64_t)a[2] << 32)] = a[0];
    return 0;
}
SHIM(steam_api, SteamAPI_UnregisterCallResult, -1) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_call_results.find((uint64_t)a[1] | ((uint64_t)a[2] << 32));
    if (it != g_call_results.end() && it->second == a[0]) g_call_results.erase(it);
    return 0;
}
SHIM(steam_api, SteamUser, -1) { return interface_object("SteamUser"); }
SHIM(steam_api, SteamFriends, -1) { return interface_object("SteamFriends"); }
SHIM(steam_api, SteamUtils, -1) { return interface_object("SteamUtils"); }
SHIM(steam_api, SteamApps, -1) { return interface_object("SteamApps"); }
SHIM(steam_api, SteamUserStats, -1) { return interface_object("SteamUserStats"); }
SHIM(steam_api, SteamNetworking, -1) { return interface_object("SteamNetworking"); }
// No game server is ever initialised, so its accessors return NULL like Valve's library does; the
// game checks for that (VA 0x0065509C).
SHIM(steam_api, SteamGameServer_Init, -1) { return 0; }
SHIM(steam_api, SteamGameServer, -1) { return 0; }
SHIM(steam_api, SteamMasterServerUpdater, -1) { return 0; }
SHIM(steam_api, SteamGameServer_Shutdown, -1) { return 0; }
SHIM(steam_api, SteamGameServer_RunCallbacks, -1) { return 0; }

} // namespace bo1
