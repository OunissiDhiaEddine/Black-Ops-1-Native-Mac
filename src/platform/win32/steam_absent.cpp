// steam_api.dll when NO Steam implementation is present (boot diagnostics, BO1_NO_STEAM_DLL=1).
// These shims answer truthfully that Steam is not there: initialisation fails, interface getters
// return NULL, callback registration is accepted and never fires. They do NOT emulate Steam (no
// user, no ownership, no stats) — by project rule (AGENTS.md ground rule 1, ADR-010) that is out of
// scope. When the shipped steam_api.dll is loaded as an emulated module the game's import slots
// point at that module instead and none of this is reached.
#include "win32_util.h"

namespace bo1 {

SHIM(steam_api, SteamAPI_Init, -1) {
    log("SteamAPI_Init -> false (no Steam implementation loaded)");
    return 0;
}
SHIM(steam_api, SteamAPI_RestartAppIfNecessary, -1) { return 0; } // "no need to relaunch through Steam"
SHIM(steam_api, SteamAPI_Shutdown, -1) { return 0; }
SHIM(steam_api, SteamAPI_RunCallbacks, -1) { return 0; }
SHIM(steam_api, SteamAPI_RegisterCallback, -1) { return 0; }
SHIM(steam_api, SteamAPI_UnregisterCallback, -1) { return 0; }
SHIM(steam_api, SteamAPI_RegisterCallResult, -1) { return 0; }
SHIM(steam_api, SteamAPI_UnregisterCallResult, -1) { return 0; }
SHIM(steam_api, SteamUser, -1) { return 0; }
SHIM(steam_api, SteamFriends, -1) { return 0; }
SHIM(steam_api, SteamUtils, -1) { return 0; }
SHIM(steam_api, SteamApps, -1) { return 0; }
SHIM(steam_api, SteamUserStats, -1) { return 0; }
SHIM(steam_api, SteamNetworking, -1) { return 0; }
SHIM(steam_api, SteamMasterServerUpdater, -1) { return 0; }
SHIM(steam_api, SteamGameServer, -1) { return 0; }
SHIM(steam_api, SteamGameServer_Init, -1) { return 0; }
SHIM(steam_api, SteamGameServer_Shutdown, -1) { return 0; }
SHIM(steam_api, SteamGameServer_RunCallbacks, -1) { return 0; }

} // namespace bo1
