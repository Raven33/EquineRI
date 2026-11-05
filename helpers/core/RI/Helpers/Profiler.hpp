#pragma once

// GPU profiler (P41, P51, P71): named, nested timestamp scopes that also open
// marker regions (P53), resolved into readback memory and read as
// milliseconds once the frame's timeline point is reached.

#include <RI/Abi.hpp>
#include <RI/RI.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace RI::inline EQUINE_RI_ABI::Helpers {

struct ScopeTime {
    std::string name;
    uint32 depth = 0;
    double milliseconds = 0.0;
};

struct Profiler {
    struct Scope {
        std::string name;
        uint32 depth = 0;
        uint32 begin = 0; // query indices
        uint32 end = 0;
    };
    Device *device = nullptr;
    QueryPool pool;
    Memory memory;
    Buffer results;
    std::vector<Scope> scopes;
    std::vector<uint32> open; // indices into scopes
    uint32 used = 0;          // queries written this frame
    bool resolved = false;
};

// Keeps &device: the device must stay at that address, alive, until destroyProfiler.
Profiler createProfiler(Device &device, uint32 maxScopes = 256);
void destroyProfiler(Profiler &profiler);

void beginScope(Profiler &profiler, CommandList &cmd, std::string_view name);
void endScope(Profiler &profiler, CommandList &cmd);
// Copies the frame's timestamps to readback memory; record before cmdEnd.
void resolveProfiler(Profiler &profiler, CommandList &cmd);
// After the frame's timeline point is reached: the scopes in begin order, then
// the profiler is ready for the next frame.
std::vector<ScopeTime> profilerResults(Profiler &profiler);

} // namespace RI::Helpers
