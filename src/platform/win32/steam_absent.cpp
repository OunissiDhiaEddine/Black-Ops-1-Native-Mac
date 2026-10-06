// steam_api.dll emulator (user-approved; the ADR-010 spike stalled because the shipped CODEX
// steam_api.dll is self-protecting and refuses to run under the interpreter). Instead of loading that
// DLL, the game's 19 Steam imports are served by these host-native stubs:
//   - SteamAPI_Init() reports Steam as present;
//   - the flat accessors SteamUser()/SteamApps()/… return fake C++ interface objects;
//   - the interface methods (ISteamUser014, ISteamApps003, …) answer "a user is logged in, app 42700
//     is owned, DLC unlocked", mirroring the game's own steam_emu.ini (AppId=42700, DLCUnlockall=1,
//     UserName=CODEX). No Steam client is contacted, nothing is emulated beyond these lies-by-design
//     needed to reach the single-player menu.
// Vtable layouts, slot order and thiscall argument counts are from re/steam_abi.csv (extracted from the
// Steamworks SDK headers the game links against — see docs/RE_NOTES.md). Steam C++ interfaces use MSVC
// thiscall (`this` in ecx); the runtime handles that (com_vtable_thiscall(), docs/HOST_ABI.md §4.3).
#include <map>
#include <string>
#include <vector>

#include "win32_util.h"

namespace bo1 {

// ---- values mirrored from artifacts/game/steam_emu.ini -----------------------------------------------
static const char *const kAppIdStr = "42700";
static const uint32_t kAppId = 42700;
static const char *const kLanguage = "english";
static const char *const kUserName = "CODEX";
static const char *const kUserDataFolder = "C:\\users\\player\\Documents\\Steam\\CODEX\\42700";

// ---- interface vtable layouts (re/steam_abi.csv -> gen/steam_abi.inc) --------------------------------
struct SteamAbi {
    const char *iface;
    int slot;
    const char *method;
    int words; // stack slots including `this`
};
static const SteamAbi kAbi[] = {
#include "steam_abi.inc"
};

// ---- thiscall method stubs. `this` is a[0]; the remaining arguments are a[1..]. -----------------------
static uint32_t m_zero(Cpu *, const uint32_t *) { return 0; }  // false / 0 / NULL — the safe default
static uint32_t m_true(Cpu *, const uint32_t *) { return 1; }  // BLoggedOn / BIsSubscribed / …

// CSteamID is returned by value in EDX:EAX (8-byte struct in registers). A valid individual account:
// universe=public(1), type=individual(1), instance=1, account 0x01100001.
static uint32_t m_steamid(Cpu *cpu, const uint32_t *) {
    cpu->edx = 0x01100001u; // (1<<24) universe | (1<<20) type | (1<<0) instance
    return 0x01100001u;     // account id
}

// GetHSteamUser returns the per-instance user handle (1 is fine: the game only checks whether it exists).
static uint32_t m_hsteamuser(Cpu *, const uint32_t *) { return 1; }

// const char * getters return a pointer into guest memory.
static uint32_t m_language(Cpu *, const uint32_t *) { return heap_strdup(kLanguage); }
static uint32_t m_username(Cpu *, const uint32_t *) { return heap_strdup(kUserName); }
static uint32_t m_userdata_folder(Cpu *, const uint32_t *) { return heap_strdup(kUserDataFolder); }
static uint32_t m_ip_country(Cpu *, const uint32_t *) { return heap_strdup("US"); }

// EUserHasLicenseForAppResult: k_EUserHasLicenseResultHasLicense == 2.
static uint32_t m_has_license(Cpu *, const uint32_t *) { return 2; }

// GetConnectedUniverse: k_EUniversePublic == 1.
static uint32_t m_universe(Cpu *, const uint32_t *) { return 1; }

// GetServerRealTime: return a plausible Unix timestamp (fixed, so savedata timestamps are stable).
static uint32_t m_servertime(Cpu *, const uint32_t *) { return 1600000000u; }

// ---- build one interface's vtable from kAbi, applying per-method overrides ---------------------------
using Overrides = std::map<std::string, ShimFn>;
static uint32_t build_vtable(const char *iface, const Overrides &ov) {
    static std::vector<std::string> names; // ComMethod holds const char*: keep them alive
    std::vector<ComMethod> methods;
    for (const SteamAbi &m : kAbi) {
        if (strcmp(m.iface, iface) != 0) continue;
        ShimFn fn = m_zero;
        auto it = ov.find(m.method);
        if (it != ov.end()) fn = it->second;
        names.push_back(m.method);
        methods.push_back({names.back().c_str(), m.words, fn});
    }
    return com_vtable_thiscall(iface, methods.data(), methods.size());
}

// ---- one cached object instance per interface (Steam accessors are singletons) -----------------------
static uint32_t obj(const char *iface, const Overrides &ov) {
    static std::map<std::string, uint32_t> cache;
    auto it = cache.find(iface);
    if (it != cache.end()) { log("  cached %s -> %08x", iface, it->second); return it->second; }
    uint32_t vt = build_vtable(iface, ov);
    log("build_vtable(%s) -> %08x", iface, vt);
    uint32_t o = com_new(vt, 0);
    log("  com_new(%s) -> %08x", iface, o);
    cache[iface] = o;
    return o;
}

static uint32_t steam_user() {
    return obj("SteamUser", {
        {"BLoggedOn", m_true},
        {"GetHSteamUser", m_hsteamuser},
        {"GetSteamID", m_steamid},
        {"GetUserDataFolder", m_userdata_folder},
        {"UserHasLicenseForApp", m_has_license},
    });
}
static uint32_t steam_apps() {
    return obj("SteamApps", {
        {"BIsSubscribed", m_true},
        {"BIsSubscribedApp", m_true},
        {"BIsDlcInstalled", m_true},
        {"GetCurrentGameLanguage", m_language},
        {"GetAvailableGameLanguages", m_language},
    });
}
static uint32_t steam_utils() {
    return obj("SteamUtils", {
        {"GetAppID", [](Cpu *, const uint32_t *) { return kAppId; }},
        {"GetConnectedUniverse", m_universe},
        {"GetServerRealTime", m_servertime},
        {"GetIPCountry", m_ip_country},
        {"IsOverlayEnabled", m_zero},
    });
}
static uint32_t steam_friends() {
    return obj("SteamFriends", {
        {"GetPersonaName", m_username},
        {"GetFriendCount", m_zero},
        {"GetClanCount", m_zero},
    });
}
static uint32_t steam_user_stats() {
    return obj("SteamUserStats", {
        {"RequestCurrentStats", m_true},
    });
}
static uint32_t steam_networking() { return obj("SteamNetworking", {}); }
static uint32_t steam_master_server_updater() { return obj("SteamMasterServerUpdater", {}); }
static uint32_t steam_game_server() { return obj("SteamGameServer", {}); }

// ---- flat imports (cdecl, from re/import_abi.csv "Steamworks S_API = cdecl") -------------------------
SHIM(steam_api, SteamAPI_Init, -1) {
    // Steam emulator: report Steam as present so the game proceeds past "Steam must be running".
    log("SteamAPI_Init -> true (Steam emulator)");
    return 1;
}
SHIM(steam_api, SteamAPI_RestartAppIfNecessary, -1) { return 0; } // no relaunch through Steam
SHIM(steam_api, SteamAPI_Shutdown, -1) { return 0; }
SHIM(steam_api, SteamAPI_RunCallbacks, -1) { return 0; }
SHIM(steam_api, SteamAPI_RegisterCallback, -1) { return 0; }
SHIM(steam_api, SteamAPI_UnregisterCallback, -1) { return 0; }
SHIM(steam_api, SteamAPI_RegisterCallResult, -1) { return 0; }
SHIM(steam_api, SteamAPI_UnregisterCallResult, -1) { return 0; }
SHIM(steam_api, SteamUser, -1) { return steam_user(); }
SHIM(steam_api, SteamFriends, -1) { return steam_friends(); }
SHIM(steam_api, SteamUtils, -1) { uint32_t o = steam_utils(); log("SteamUtils -> %08x", o); return o; }
SHIM(steam_api, SteamApps, -1) { return steam_apps(); }
SHIM(steam_api, SteamUserStats, -1) { return steam_user_stats(); }
SHIM(steam_api, SteamNetworking, -1) { return steam_networking(); }
SHIM(steam_api, SteamMasterServerUpdater, -1) { return steam_master_server_updater(); }
SHIM(steam_api, SteamGameServer, -1) { return steam_game_server(); }
SHIM(steam_api, SteamGameServer_Init, -1) { return 0; } // no dedicated/game server
SHIM(steam_api, SteamGameServer_Shutdown, -1) { return 0; }
SHIM(steam_api, SteamGameServer_RunCallbacks, -1) { return 0; }

} // namespace bo1
