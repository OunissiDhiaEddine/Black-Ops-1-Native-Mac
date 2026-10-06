// Trace replay harness (T-031).
//   replay <trace.bin> [--backend null|metal] [--frames A-B] [--dump DIR] [--window] [--fps N]
// Feeds the game's recorded D3D9 calls (depth 0 only) to a backend, frame by frame.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "backend.h"

using namespace bo1;

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: replay <trace.bin> [--backend null|metal] [--frames A-B] [--dump DIR] [--window] [--fps N]\n");
        return 2;
    }
    std::string path = argv[1], backend_name = "null";
    replay::Options opts;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--backend" && i + 1 < argc) backend_name = argv[++i];
        else if (a == "--frames" && i + 1 < argc) {
            const char *s = argv[++i];
            opts.first_frame = std::atoi(s);
            if (const char *dash = std::strchr(s, '-')) opts.last_frame = std::atoi(dash + 1);
        } else if (a == "--dump" && i + 1 < argc) opts.dump_dir = argv[++i];
        else if (a == "--window") opts.window = true, backend_name = "metal";
        else if (a == "--fps" && i + 1 < argc) opts.fps = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    try {
        trace::Reader reader(path);
        std::unique_ptr<replay::Backend> backend;
        if (backend_name == "null") backend = replay::make_null_backend(opts);
        else if (backend_name == "metal") backend = replay::make_metal_backend(opts);
        else {
            std::fprintf(stderr, "unknown backend %s\n", backend_name.c_str());
            return 2;
        }
        auto t0 = std::chrono::steady_clock::now();
        trace::Record r;
        uint64_t records = 0, nested = 0;
        int frame = 0;
        while (!backend->wants_quit() && reader.next(r)) {
            records++;
            if (r.depth) { // made by the D3D9 implementation while servicing a game call
                nested++;
                continue;
            }
            backend->call(r, frame);
            if (r.iface == IFACE_SWAPCHAIN && r.slot == 3) backend->end_frame(++frame);
            else if (r.iface == IFACE_DEVICE && r.slot == 17) backend->end_frame(++frame);
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s: %llu records (%llu nested skipped), %.1f MiB, read in %.2f s (%.1f M records/s)%s\n", path.c_str(),
                    (unsigned long long)records, (unsigned long long)nested, reader.size() / 1048576.0, secs,
                    records / secs / 1e6, reader.truncated() ? "  [truncated tail]" : "");
        if (reader.version() < 2)
            std::printf("note: v1 trace — nesting depth is not exact; expect false 'unknown object' reports\n");
        int problems = backend->report();
        return problems ? 1 : 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
