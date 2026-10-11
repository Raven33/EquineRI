// Implements Profiler.hpp (P41, P51): timestamp scopes with marker regions, on
// RI's public functions.

#include <RI/Helpers/Profiler.hpp>

#include <cstring>

namespace RI::Helpers {

Profiler createProfiler(Device &device, uint32 maxScopes) {
    Profiler profiler;
    profiler.device = &device;
    profiler.pool = createQueryPool(device, {.type = QueryType::eTimestamp, .count = maxScopes * 2});
    const BufferDesc desc{.size = uint64{maxScopes} * 2 * 8, .usage = BufferUsage::eCopyDst};
    profiler.memory = allocateMemory(device, MemoryKind::eReadback, bufferRequirements(device, desc).size);
    profiler.results = createBuffer(device, profiler.memory, 0, desc);
    return profiler;
}

void destroyProfiler(Profiler &profiler) {
    destroyBuffer(*profiler.device, profiler.results);
    freeMemory(*profiler.device, profiler.memory);
    destroyQueryPool(*profiler.device, profiler.pool);
    profiler = {};
}

void beginScope(Profiler &profiler, CommandList &cmd, std::string_view name) {
    if (profiler.used + 2 > profiler.pool.count) {
        reportError(ErrorCategory::eProgrammerError, "Helpers::beginScope: more scopes than createProfiler allows");
    }
    const Profiler::Scope scope{std::string(name), static_cast<uint32>(profiler.open.size()), profiler.used++, 0};
    profiler.open.push_back(static_cast<uint32>(profiler.scopes.size()));
    profiler.scopes.push_back(scope);
    cmdBeginMarker(cmd, name);
    cmdWriteTimestamp(cmd, profiler.pool, scope.begin);
}

void endScope(Profiler &profiler, CommandList &cmd) {
    if (profiler.open.empty()) {
        reportError(ErrorCategory::eProgrammerError, "Helpers::endScope: no scope is open");
    }
    Profiler::Scope &scope = profiler.scopes[profiler.open.back()];
    profiler.open.pop_back();
    scope.end = profiler.used++;
    cmdWriteTimestamp(cmd, profiler.pool, scope.end);
    cmdEndMarker(cmd);
}

void resolveProfiler(Profiler &profiler, CommandList &cmd) {
    if (!profiler.open.empty()) {
        reportError(ErrorCategory::eProgrammerError, "Helpers::resolveProfiler: a scope is still open");
    }
    if (profiler.used > 0) {
        cmdResolveQueries(cmd, profiler.pool, 0, profiler.used, profiler.results, 0);
    }
    profiler.resolved = true;
}

std::vector<ScopeTime> profilerResults(Profiler &profiler) {
    std::vector<ScopeTime> times;
    if (!profiler.resolved) {
        return times;
    }
    const auto *ticks = static_cast<const uint64 *>(profiler.results.mapped);
    const double toMs = 1000.0 / static_cast<double>(profiler.device->limits.timestampFrequency);
    for (const Profiler::Scope &scope : profiler.scopes) {
        times.push_back({scope.name, scope.depth, static_cast<double>(ticks[scope.end] - ticks[scope.begin]) * toMs});
    }
    if (profiler.used > 0) {
        resetQueries(*profiler.device, profiler.pool, 0, profiler.used);
    }
    profiler.scopes.clear();
    profiler.used = 0;
    profiler.resolved = false;
    return times;
}

} // namespace RI::Helpers
