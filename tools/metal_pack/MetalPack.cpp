// Build-time tool (P114): appends a metallib to a DXIL container as an `MTLB`
// part, giving the `.mtl.dxil` file the Metal backend consumes. RI reads the
// shader's facts from the DXIL parts and creates the library from `MTLB`.
// For geometry and tessellation emulation (P114, P116 amended 2026-11-09) it
// also appends `MTLR`, the numbers of MSC's reflection the emulation needs
// (see `kReflectionFields`), and `MTLS`, a vertex shader's stage-in metallib.
//
//   equine_metal_pack <in.dxil> <in.metallib> <out.mtl.dxil>
//                     [<reflection.json> [<stage-in.metallib>]]
//
// MTLR is little-endian uint32 values, in kReflectionFields order after a
// version (1); a field a stage lacks is 0. The JSON is MSC 4's reflection
// file: only number and string values of its "state" object are read.
//
// The container's hash is left as it was: the file is for Metal only, and a
// D3D12 runtime would refuse it.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
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
constexpr std::uint32_t kPartMTLR = fourCC('M', 'T', 'L', 'R');
constexpr std::uint32_t kPartMTLS = fourCC('M', 'T', 'L', 'S');

// MTLR's fields, in order; RI reads them as RI::Metal::EmulationReflection
// (backends/metal/DeviceState.hpp), which must match. `stage` is MSC's
// "ShaderType"; `key` its "state" member.
struct ReflectionField {
    const char *stage;
    const char *key;
};
constexpr ReflectionField kReflectionFields[] = {
    {"Vertex", "vertex_output_size_in_bytes"},
    {"Geom", "max_input_primitives_per_mesh_threadgroup"},
    {"Geom", "instance_count"},
    {"Hull", "max_patches_per_object_threadgroup"},
    {"Hull", "input_control_point_count"},
    {"Hull", "max_object_threads_per_patch"},
    {"Hull", "max_tessellation_factor"},     // a float, stored as its bits
    {"Hull", "tessellator_output_primitive"}, // IRTessellatorOutputPrimitive
    {"Domain", "max_input_prims_per_mesh_threadgroup"},
};
constexpr std::uint32_t kReflectionVersion = 1;

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

struct Part {
    std::uint32_t code;
    std::vector<std::byte> data;
};

// The text of `"key":` 's value in `json` (a number, or a string without its
// quotes), or nullopt.
std::optional<std::string> jsonValue(const std::string &json, const std::string &key) {
    const std::string needle = "\"" + key + "\":";
    const std::size_t at = json.find(needle);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    std::size_t begin = at + needle.size();
    while (begin < json.size() && json[begin] == ' ') {
        ++begin;
    }
    if (begin < json.size() && json[begin] == '"') {
        const std::size_t end = json.find('"', begin + 1);
        return end == std::string::npos ? std::nullopt : std::optional(json.substr(begin + 1, end - begin - 1));
    }
    const std::size_t end = json.find_first_of(",}]", begin);
    return end == std::string::npos ? std::nullopt : std::optional(json.substr(begin, end - begin));
}

// MTLR from MSC's reflection; empty string on success, else the reason.
std::string reflectionPart(const std::string &json, std::vector<std::byte> &out) {
    const std::optional<std::string> stage = jsonValue(json, "ShaderType");
    const std::size_t state = json.find("\"state\":");
    if (!stage || state == std::string::npos) {
        return "the reflection file has no ShaderType or state";
    }
    const std::string stateJson = json.substr(state);
    out.clear();
    appendU32(out, kReflectionVersion);
    for (const ReflectionField &field : kReflectionFields) {
        std::uint32_t value = 0;
        if (*stage == field.stage) {
            const std::optional<std::string> text = jsonValue(stateJson, field.key);
            if (!text) {
                return std::string("the reflection has no state.") + field.key;
            }
            const std::string key = field.key;
            if (key == "max_tessellation_factor") {
                const float factor = std::stof(*text);
                std::memcpy(&value, &factor, sizeof(value));
            } else if (key == "tessellator_output_primitive") {
                const std::string &name = *text;
                value = name == "Point" ? 1 : name == "Line" ? 2 : name == "TriangleCW" ? 3 : name == "TriangleCCW" ? 4 : 0;
                if (value == 0) {
                    return "unknown tessellator_output_primitive " + name;
                }
            } else {
                value = static_cast<std::uint32_t>(std::stoul(*text));
            }
        }
        appendU32(out, value);
    }
    return {};
}

// Returns an empty string on success, else the reason.
std::string pack(std::span<const std::byte> dxil, const std::vector<Part> &parts, std::vector<std::byte> &out) {
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
        const std::uint32_t code = loadU32(dxil, offset);
        if (code == kPartMTLB || code == kPartMTLR || code == kPartMTLS) {
            return "the container already has a Metal part";
        }
    }
    // The part table grows by one entry per new part: existing parts move.
    const std::size_t shift = 4 * parts.size();
    std::size_t newSize = size + shift;
    for (const Part &part : parts) {
        newSize += kPartHeaderSize + ((part.data.size() + 3) & ~std::size_t{3});
    }
    if (newSize > UINT32_MAX) {
        return "the packed container would exceed 4 GiB";
    }

    out.assign(dxil.begin(), dxil.begin() + static_cast<std::ptrdiff_t>(kContainerHeaderSize));
    storeU32(out, kSizeOffset, static_cast<std::uint32_t>(newSize));
    storeU32(out, kPartCountOffset, static_cast<std::uint32_t>(partCount + parts.size()));
    for (std::size_t i = 0; i < partCount; ++i) {
        appendU32(out, loadU32(dxil, kContainerHeaderSize + (4 * i)) + static_cast<std::uint32_t>(shift));
    }
    std::size_t next = size + shift;
    for (const Part &part : parts) {
        appendU32(out, static_cast<std::uint32_t>(next));
        next += kPartHeaderSize + ((part.data.size() + 3) & ~std::size_t{3});
    }
    out.insert(out.end(), dxil.begin() + static_cast<std::ptrdiff_t>(tableEnd), dxil.end());
    for (const Part &part : parts) {
        const std::size_t padded = (part.data.size() + 3) & ~std::size_t{3};
        appendU32(out, part.code);
        appendU32(out, static_cast<std::uint32_t>(padded));
        out.insert(out.end(), part.data.begin(), part.data.end());
        out.resize(out.size() + padded - part.data.size());
    }
    return {};
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4 || argc > 6) {
        std::fputs("usage: equine_metal_pack <in.dxil> <in.metallib> <out.mtl.dxil> [<reflection.json> "
                   "[<stage-in.metallib>]]\n",
                   stderr);
        return 2;
    }
    const std::span<char *> args(argv, static_cast<std::size_t>(argc));
    const std::optional<std::vector<std::byte>> dxil = readFile(args[1]);
    const std::optional<std::vector<std::byte>> metallib = readFile(args[2]);
    if (!dxil || !metallib) {
        std::fprintf(stderr, "equine_metal_pack: cannot read %s\n", !dxil ? args[1] : args[2]);
        return 1;
    }
    std::vector<Part> parts{{kPartMTLB, *metallib}};
    if (argc >= 5) {
        const std::optional<std::vector<std::byte>> json = readFile(args[4]);
        if (!json) {
            std::fprintf(stderr, "equine_metal_pack: cannot read %s\n", args[4]);
            return 1;
        }
        std::string text(json->size(), '\0');
        std::memcpy(text.data(), json->data(), json->size());
        Part reflection{kPartMTLR, {}};
        if (const std::string error = reflectionPart(text, reflection.data); !error.empty()) {
            std::fprintf(stderr, "equine_metal_pack: %s: %s\n", args[4], error.c_str());
            return 1;
        }
        parts.push_back(std::move(reflection));
    }
    if (argc == 6) {
        const std::optional<std::vector<std::byte>> stageIn = readFile(args[5]);
        if (!stageIn) {
            std::fprintf(stderr, "equine_metal_pack: cannot read %s\n", args[5]);
            return 1;
        }
        parts.push_back({kPartMTLS, *stageIn});
    }
    std::vector<std::byte> out;
    if (const std::string error = pack(*dxil, parts, out); !error.empty()) {
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
