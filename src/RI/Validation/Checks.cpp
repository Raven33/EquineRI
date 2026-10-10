#include <RI/Validation/Checks.hpp>

#include <format>

namespace RI::Validation {

CheckResult checkSlot(uint32 slot, uint32 count, std::string_view heap) {
    if (slot >= count) {
        return std::format("slot {} is outside the {} heap (it has {} slots)", slot, heap, count);
    }
    return {};
}

CheckResult checkRootDataSize(uint64 given, uint32 declared) {
    if (declared == 0) {
        return std::format("root data of {} bytes set, but the pipeline's shader declares none "
                           "(mark it with EQ_ROOT_DATA)",
                           given);
    }
    if (given != declared) {
        return std::format("root data is {} bytes, but the pipeline's shader declares {} bytes", given, declared);
    }
    return {};
}

CheckResult checkCommandListIdle(uint64 lastSubmitted, uint64 completed, std::string_view action) {
    if (lastSubmitted > completed) {
        return std::format("{} a command list the GPU may still be executing: wait for its timeline point "
                           "(value {}) first; the queue has reached {}",
                           action, lastSubmitted, completed);
    }
    return {};
}

CheckResult checkRange(uint64 offset, uint64 size, uint64 total, uint64 alignment, std::string_view what) {
    if (offset > total || size > total - offset) {
        return std::format("{}: range [{}, {}) exceeds the {} bytes available", what, offset, offset + size, total);
    }
    if (alignment != 0 && offset % alignment != 0) {
        return std::format("{}: offset {} is not a multiple of the required alignment {}", what, offset, alignment);
    }
    return {};
}

CheckResult checkBufferView(uint64 offset, uint64 size, uint64 bufferSize, uint32 stride, bool constant) {
    if (constant) {
        if (CheckResult r = checkRange(offset, constantViewSize(size), bufferSize, 256,
                                       "constant buffer view (size rounded up to 256 bytes)")) {
            return r;
        }
        return {};
    }
    if (CheckResult r = checkRange(offset, size, bufferSize, 16, "buffer view")) {
        return r;
    }
    const uint64 element = stride == 0 ? 4 : stride;
    if (offset % element != 0 || size % element != 0 || size == 0) {
        return std::format("buffer view: offset {} and size {} must be non-empty multiples of the element size {}",
                           offset, size, element);
    }
    return {};
}

} // namespace RI::Validation
