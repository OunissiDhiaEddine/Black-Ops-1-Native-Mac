// T-047 (ADR-014): can the real Steam client on this Mac serve Black Ops (app id 42700)?
//   scripts/steam_spike.sh        (needs Steam running + Valve's libsteam_api.dylib, see the script)
// Uses only C symbols exported by libsteam_api.dylib (looked up by name, so no SDK headers are needed
// and a missing symbol is reported instead of failing the build). Read-only: initialises the API, asks
// who is logged on and whether the account owns the app, asks for the interface versions the 2010 game
// was built against, shuts down.
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APP_ID 42700u

static void *lib;
static void *sym(const char *name) { return dlsym(lib, name); }
// newest accessor of an interface: SteamAPI_SteamUser_v0NN, highest NN that exists
static void *latest(const char *base, char *found, size_t n) {
    for (int v = 40; v >= 1; v--) {
        snprintf(found, n, "%s_v%03d", base, v);
        void *(*get)(void) = (void *(*)(void))sym(found);
        if (get) return get();
    }
    found[0] = 0;
    return NULL;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "libsteam_api.dylib";
    lib = dlopen(path, RTLD_NOW);
    if (!lib) {
        printf("cannot load %s: %s\n", path, dlerror());
        return 2;
    }
    bool ok = false;
    bool (*init)(void) = (bool (*)(void))sym("SteamAPI_Init");
    int (*init_flat)(char *) = (int (*)(char *))sym("SteamAPI_InitFlat");
    char err[1024] = "";
    if (init_flat) {
        int rc = init_flat(err); // 0 = k_ESteamAPIInitResult_OK
        ok = rc == 0;
        printf("SteamAPI_InitFlat -> %d %s\n", rc, err);
    } else if (init) {
        ok = init();
        printf("SteamAPI_Init -> %s\n", ok ? "true" : "false");
    } else {
        printf("neither SteamAPI_Init nor SteamAPI_InitFlat is exported\n");
    }
    if (!ok) {
        printf("RESULT: Steam did not initialise for app %u (is Steam running and logged in? steam_appid.txt present?)\n", APP_ID);
        return 1;
    }
    char name[96];
    void *user = latest("SteamAPI_SteamUser", name, sizeof(name));
    bool (*logged)(void *) = (bool (*)(void *))sym("SteamAPI_ISteamUser_BLoggedOn");
    uint64_t (*steam_id)(void *) = (uint64_t (*)(void *))sym("SteamAPI_ISteamUser_GetSteamID");
    if (user && logged) printf("logged on: %s (accessor %s)\n", logged(user) ? "yes" : "no", name);
    if (user && steam_id) printf("steam id: present (%s)\n", steam_id(user) ? "non-zero" : "zero"); // the id itself is not printed
    void *apps = latest("SteamAPI_SteamApps", name, sizeof(name));
    bool (*subscribed_app)(void *, uint32_t) = (bool (*)(void *, uint32_t))sym("SteamAPI_ISteamApps_BIsSubscribedApp");
    bool (*subscribed)(void *) = (bool (*)(void *))sym("SteamAPI_ISteamApps_BIsSubscribed");
    int owned = -1;
    if (apps && subscribed_app) owned = subscribed_app(apps, APP_ID);
    else if (apps && subscribed) owned = subscribed(apps);
    printf("account owns app %u: %s\n", APP_ID, owned < 0 ? "could not ask" : owned ? "yes" : "NO");

    // interface versions of the game's SDK (strings from Valve's original 2010 steam_api.dll)
    int32_t (*huser)(void) = (int32_t (*)(void))sym("SteamAPI_GetHSteamUser");
    void *(*find_user_if)(int32_t, const char *) = (void *(*)(int32_t, const char *))sym("SteamInternal_FindOrCreateUserInterface");
    static const char *const versions[] = {"SteamUser014", "SteamFriends007", "SteamUtils005", "STEAMAPPS_INTERFACE_VERSION003",
                                           "STEAMUSERSTATS_INTERFACE_VERSION007", "SteamNetworking004", "SteamMatchMaking008"};
    int served = 0, asked = 0;
    if (huser && find_user_if) {
        for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
            void *p = find_user_if(huser(), versions[i]);
            printf("  %-40s %s\n", versions[i], p ? "served" : "NOT served");
            asked++, served += p != NULL;
        }
    } else {
        printf("SteamInternal_FindOrCreateUserInterface is not exported: old interface versions cannot be requested this way\n");
    }
    void (*shutdown)(void) = (void (*)(void))sym("SteamAPI_Shutdown");
    if (shutdown) shutdown();
    printf("RESULT: init ok, owned=%d, old interfaces served %d/%d\n", owned, served, asked);
    return owned == 1 && asked && served == asked ? 0 : 1;
}
