// sm3c — translate D3D9 shader blobs to MSL and (optionally) compile them with Metal.
//   sm3c [--out DIR] [--no-compile] [--shadow MASK] [--report FILE.json] <blob or directory>...
// Used to measure the translator against the real shader corpus (artifacts/shaders/*).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sm3.h"

using namespace bo1;

int main(int argc, char **argv) {
    @autoreleasepool {
        std::vector<std::string> inputs;
        std::string out_dir, report;
        bool compile = true;
        sm3::Options opts;
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
            else if (a == "--report" && i + 1 < argc) report = argv[++i];
            else if (a == "--no-compile") compile = false;
            else if (a == "--shadow" && i + 1 < argc) opts.shadow_samplers = (uint32_t)strtoul(argv[++i], nullptr, 0);
            else inputs.push_back(a);
        }
        if (inputs.empty()) {
            fprintf(stderr, "usage: sm3c [--out DIR] [--no-compile] [--shadow MASK] [--report FILE.json] <blob|dir>...\n");
            return 2;
        }
        NSFileManager *fm = NSFileManager.defaultManager;
        std::vector<std::string> files;
        for (auto &in : inputs) {
            BOOL dir = NO;
            NSString *p = @(in.c_str());
            if (![fm fileExistsAtPath:p isDirectory:&dir]) {
                fprintf(stderr, "no such file: %s\n", in.c_str());
                return 2;
            }
            if (!dir) {
                files.push_back(in);
                continue;
            }
            for (NSString *name in [[fm contentsOfDirectoryAtPath:p error:nil] sortedArrayUsingSelector:@selector(compare:)])
                if ([name hasSuffix:@".vs"] || [name hasSuffix:@".ps"]) files.push_back(in + "/" + name.UTF8String);
        }
        if (!out_dir.empty()) [fm createDirectoryAtPath:@(out_dir.c_str()) withIntermediateDirectories:YES attributes:nil error:nil];
        id<MTLDevice> device = compile ? MTLCreateSystemDefaultDevice() : nil;
        if (compile && !device) {
            fprintf(stderr, "no Metal device\n");
            return 2;
        }

        // Blocks capture C++ locals by const copy, so the shared state lives behind a pointer.
        struct Shared {
            std::mutex mu;
            std::map<std::string, int> translate_errors, compile_errors;
            std::map<std::string, std::string> example; // error -> first file
            std::atomic<int> ok_translate{0}, ok_compile{0}, n_vs{0}, n_ps{0};
            std::atomic<long long> msl_bytes{0}, insts{0};
        } shared;
        Shared *sh = &shared;
        const std::vector<std::string> *filesp = &files;
        const sm3::Options *optsp = &opts;
        const std::string *out_dirp = &out_dir;
        auto t0 = std::chrono::steady_clock::now();
        dispatch_apply(files.size(), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
            @autoreleasepool {
                const std::string &path = (*filesp)[i];
                NSData *d = [NSData dataWithContentsOfFile:@(path.c_str())];
                std::span<const uint32_t> code((const uint32_t *)d.bytes, d.length / 4);
                sm3::Result r = sm3::translate(code, *optsp);
                (r.stage == sm3::Stage::Vertex ? sh->n_vs : sh->n_ps)++;
                if (!r.ok) {
                    std::lock_guard<std::mutex> lock(sh->mu);
                    if (!sh->translate_errors[r.error]++) sh->example[r.error] = path;
                    return;
                }
                sh->ok_translate++;
                sh->msl_bytes += (long long)r.msl.size();
                sh->insts += r.instruction_count;
                if (!out_dirp->empty()) {
                    NSString *name = [[@(path.c_str()) lastPathComponent] stringByAppendingString:@".metal"];
                    [@(r.msl.c_str()) writeToFile:[@(out_dirp->c_str()) stringByAppendingPathComponent:name]
                                       atomically:NO encoding:NSUTF8StringEncoding error:nil];
                }
                if (!compile) return;
                NSError *err = nil;
                id<MTLLibrary> lib = [device newLibraryWithSource:@(r.msl.c_str()) options:nil error:&err];
                if (lib && [lib newFunctionWithName:@(optsp->entry.c_str())]) {
                    sh->ok_compile++;
                    return;
                }
                std::string msg = err ? err.localizedDescription.UTF8String : "no entry function";
                // keep the first diagnostic line, without the line/column prefix
                size_t e = msg.find("error:");
                if (e != std::string::npos) msg = msg.substr(e);
                size_t nl = msg.find('\n');
                if (nl != std::string::npos) msg.resize(nl);
                std::lock_guard<std::mutex> lock(sh->mu);
                if (!sh->compile_errors[msg]++) sh->example[msg] = path;
            }
        });
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        int total = (int)files.size();
        printf("shaders %d (vs %d, ps %d)   translated %d (%.2f%%)", total, shared.n_vs.load(), shared.n_ps.load(), shared.ok_translate.load(),
               total ? 100.0 * shared.ok_translate / total : 0.0);
        if (compile) printf("   compiled by Metal %d (%.2f%%)", shared.ok_compile.load(), total ? 100.0 * shared.ok_compile / total : 0.0);
        printf("   %.1f s wall, %lld instructions, %.1f MiB of MSL\n", secs, shared.insts.load(), shared.msl_bytes.load() / 1048576.0);
        auto dump = [&](const char *title, std::map<std::string, int> &m) {
            if (m.empty()) return;
            std::vector<std::pair<int, std::string>> v;
            for (auto &[k, n] : m) v.push_back({n, k});
            std::sort(v.rbegin(), v.rend());
            printf("%s (%zu distinct):\n", title, v.size());
            for (size_t i = 0; i < v.size() && i < 25; i++)
                printf("  %6d  %s   e.g. %s\n", v[i].first, v[i].second.c_str(), shared.example[v[i].second].c_str());
        };
        dump("translation failures", shared.translate_errors);
        dump("Metal compile failures", shared.compile_errors);
        if (!report.empty()) {
            FILE *f = fopen(report.c_str(), "w");
            if (f) {
                fprintf(f, "{\n \"shaders\": %d, \"vs\": %d, \"ps\": %d, \"translated\": %d, \"compiled\": %d,\n", total, shared.n_vs.load(),
                        shared.n_ps.load(), shared.ok_translate.load(), compile ? shared.ok_compile.load() : -1);
                fprintf(f, " \"seconds\": %.2f, \"instructions\": %lld, \"msl_bytes\": %lld,\n \"translate_errors\": {", secs,
                        shared.insts.load(), shared.msl_bytes.load());
                bool first = true;
                for (auto &[k, n] : shared.translate_errors) fprintf(f, "%s\n  \"%s\": %d", first ? "" : ",", k.c_str(), n), first = false;
                fprintf(f, "\n },\n \"compile_error_kinds\": %zu\n}\n", shared.compile_errors.size());
                fclose(f);
            }
        }
        return (shared.ok_translate == total && (!compile || shared.ok_compile == total)) ? 0 : 1;
    }
}
