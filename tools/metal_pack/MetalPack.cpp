// Build-time tool (P114): appends a metallib to a DXIL container as an `MTLB`
// part, giving the `.mtl.dxil` file the Metal backend consumes. RI reads the
// shader's facts from the DXIL parts and creates the library from `MTLB`.
//
//   equine_metal_pack <in.dxil> <in.metallib> <out.mtl.dxil>
//
// The container's hash is left as it was: the file is for Metal only, and a
// D3D12 runtime would refuse it.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kContainerHeaderSize = 32;
constexpr std::size_t kPartHeaderSize = 8;
constexpr std::uint32_t kPartCountOffset = 28;
constexpr std::uint32_t kSizeOffset = 24;

constexpr std::uint32_t fourCC(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

constexpr std::uint32_t kMagic = fourCC('D', 'X', 'B', 'C');
constexpr std::uint32_t kPartMTLB = fourCC('M', 'T', 'L', 'B');

std::optional<std::vector<std::byte>> readFile(const char *path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        bytes[i] = static_cast<std::byte>(chars[i]);
    }
    return bytes;
}

std::uint32_t loadU32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

void storeU32(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
}

void appendU32(std::vector<std::byte> &bytes, std::uint32_t value) {
    bytes.resize(bytes.size() + 4);
    storeU32(bytes, bytes.size() - 4, value);
}

// Returns an empty string on success, else the reason.
std::string pack(std::span<const std::byte> dxil, std::span<const std::byte> metallib, std::vector<std::byte> &out) {
    if (dxil.size() < kContainerHeaderSize || loadU32(dxil, 0) != kMagic) {
        return "the input is not a DXBC container";
    }
    const std::size_t size = loadU32(dxil, kSizeOffset);
    const std::size_t partCount = loadU32(dxil, kPartCountOffset);
    const std::size_t tableEnd = kContainerHeaderSize + (4 * partCount);
    if (size != dxil.size() || tableEnd > size) {
        return "the DXBC container's size or part table is inconsistent";
    }
    for (std::size_t i = 0; i < partCount; ++i) {
        const std::size_t offset = loadU32(dxil, kContainerHeaderSize + (4 * i));
        if (offset < tableEnd || offset + kPartHeaderSize > size) {
            return "a DXBC part lies outside the container";
        }
        if (loadU32(dxil, offset) == kPartMTLB) {
            return "the container already has an MTLB part";
        }
    }
    const std::size_t padded = (metallib.size() + 3) & ~std::size_t{3};
    const std::size_t newSize = size + 4 + kPartHeaderSize + padded;
    if (newSize > UINT32_MAX) {
        return "the packed container would exceed 4 GiB";
    }

    // Header and a part table one entry longer; existing parts move by 4 bytes.
    out.assign(dxil.begin(), dxil.begin() + static_cast<std::ptrdiff_t>(kContainerHeaderSize));
    storeU32(out, kSizeOffset, static_cast<std::uint32_t>(newSize));
    storeU32(out, kPartCountOffset, static_cast<std::uint32_t>(partCount + 1));
    for (std::size_t i = 0; i < partCount; ++i) {
        appendU32(out, loadU32(dxil, kContainerHeaderSize + (4 * i)) + 4);
    }
    appendU32(out, static_cast<std::uint32_t>(size + 4));
    out.insert(out.end(), dxil.begin() + static_cast<std::ptrdiff_t>(tableEnd), dxil.end());
    appendU32(out, kPartMTLB);
    appendU32(out, static_cast<std::uint32_t>(padded));
    out.insert(out.end(), metallib.begin(), metallib.end());
    out.resize(newSize);
    return {};
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fputs("usage: equine_metal_pack <in.dxil> <in.metallib> <out.mtl.dxil>\n", stderr);
        return 2;
    }
    const std::span<char *> args(argv, static_cast<std::size_t>(argc));
    const std::optional<std::vector<std::byte>> dxil = readFile(args[1]);
    const std::optional<std::vector<std::byte>> metallib = readFile(args[2]);
    if (!dxil || !metallib) {
        std::fprintf(stderr, "equine_metal_pack: cannot read %s\n", !dxil ? args[1] : args[2]);
        return 1;
    }
    std::vector<std::byte> out;
    if (const std::string error = pack(*dxil, *metallib, out); !error.empty()) {
        std::fprintf(stderr, "equine_metal_pack: %s: %s\n", args[1], error.c_str());
        return 1;
    }
    std::ofstream file(args[3], std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) {
        std::fprintf(stderr, "equine_metal_pack: cannot write %s\n", args[3]);
        return 1;
    }
    return 0;
}
