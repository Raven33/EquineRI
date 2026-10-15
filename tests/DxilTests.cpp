// GPU-free tests of RI's DXIL reader (P67) on shaders compiled at build time by
// the pinned DXC (P93, reflection and debug info stripped), plus synthetic and
// corrupted containers. Malformed input must give an error, never a crash
// (these run under ASan + UBSan).

#include <TestHarness.hpp>

#include <RI/Reflection/Dxil.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
using RI::Reflection::DxilBinding;
using RI::Reflection::DxilReadResult;
using RI::Reflection::DxilResourceClass;
using RI::Reflection::readDxil;

Bytes loadShader(const char *name) {
    std::ifstream file(std::string(EQUINE_SHADER_DIR) + "/" + name + ".dxil", std::ios::binary);
    Bytes bytes;
    for (auto it = std::istreambuf_iterator<char>(file); it != std::istreambuf_iterator<char>(); ++it) {
        bytes.push_back(static_cast<std::byte>(*it));
    }
    EQ_CHECK(!bytes.empty());
    return bytes;
}

DxilReadResult read(const Bytes &bytes) { return readDxil(std::span<const std::byte>(bytes)); }

uint32 loadU32(const Bytes &bytes, size_t offset) {
    uint32 value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}

void storeU32(Bytes &bytes, size_t offset, uint32 value) { std::memcpy(bytes.data() + offset, &value, 4); }

void appendU32(Bytes &bytes, uint32 value) {
    bytes.resize(bytes.size() + 4);
    storeU32(bytes, bytes.size() - 4, value);
}

constexpr uint32 fourCC(const char (&name)[5]) {
    return uint32(uint8(name[0])) | (uint32(uint8(name[1])) << 8) | (uint32(uint8(name[2])) << 16) |
           (uint32(uint8(name[3])) << 24);
}

// A container split into its parts, and put back together.
struct Part {
    uint32 code = 0;
    Bytes data;
    size_t offset = 0; // of the part's data in the original container
};

std::vector<Part> splitParts(const Bytes &container) {
    std::vector<Part> parts;
    const uint32 count = loadU32(container, 28);
    for (uint32 i = 0; i < count; ++i) {
        const uint32 offset = loadU32(container, 32 + 4 * i);
        const uint32 size = loadU32(container, offset + 4);
        Part part;
        part.code = loadU32(container, offset);
        part.offset = offset + 8;
        part.data.assign(container.begin() + offset + 8, container.begin() + offset + 8 + size);
        parts.push_back(std::move(part));
    }
    return parts;
}

Bytes joinParts(const std::vector<Part> &parts) {
    Bytes container(32 + 4 * parts.size());
    storeU32(container, 0, fourCC("DXBC"));
    storeU32(container, 20, 1); // version 1.0
    storeU32(container, 28, static_cast<uint32>(parts.size()));
    for (size_t i = 0; i < parts.size(); ++i) {
        storeU32(container, 32 + 4 * i, static_cast<uint32>(container.size()));
        appendU32(container, parts[i].code);
        appendU32(container, static_cast<uint32>(parts[i].data.size()));
        container.insert(container.end(), parts[i].data.begin(), parts[i].data.end());
    }
    storeU32(container, 24, static_cast<uint32>(container.size()));
    return container;
}

Part *findPart(std::vector<Part> &parts, uint32 code) {
    for (Part &part : parts) {
        if (part.code == code) {
            return &part;
        }
    }
    return nullptr;
}

// Rewrites PSV0 with a different runtime-info size and resource record size
// (zero padded or cut), keeping everything after the records.
Bytes resizePsv(const Bytes &psv, uint32 runtimeInfoSize, uint32 recordSize) {
    const uint32 oldInfoSize = loadU32(psv, 0);
    size_t offset = 4 + oldInfoSize;
    const uint32 count = loadU32(psv, offset);
    offset += 4;
    const uint32 oldRecordSize = count > 0 ? loadU32(psv, offset) : 0;
    offset += count > 0 ? 4 : 0;

    Bytes out;
    appendU32(out, runtimeInfoSize);
    for (uint32 i = 0; i < runtimeInfoSize; ++i) {
        out.push_back(i < oldInfoSize ? psv[4 + i] : std::byte{0});
    }
    appendU32(out, count);
    if (count > 0) {
        appendU32(out, recordSize);
    }
    for (uint32 r = 0; r < count; ++r, offset += oldRecordSize) {
        for (uint32 i = 0; i < recordSize; ++i) {
            out.push_back(i < oldRecordSize ? psv[offset + i] : std::byte{0});
        }
    }
    out.insert(out.end(), psv.begin() + static_cast<std::ptrdiff_t>(offset), psv.end());
    return out;
}

bool hasBinding(const DxilReadResult &result, DxilResourceClass resourceClass, uint32 space, uint32 lowerBound,
                uint32 count) {
    for (const DxilBinding &binding : result.info.bindings) {
        if (binding.resourceClass == resourceClass && binding.space == space && binding.lowerBound == lowerBound &&
            binding.count == count) {
            return true;
        }
    }
    return false;
}

bool threadGroupIs(const DxilReadResult &result, uint32 x, uint32 y, uint32 z) {
    return result.info.threadGroupSize[0] == x && result.info.threadGroupSize[1] == y &&
           result.info.threadGroupSize[2] == z;
}

bool mentions(const DxilReadResult &result, const char *text) { return result.error.find(text) != std::string::npos; }

// A read of corrupted input must either fail with a message or produce a result
// whose root data is consistent; it must never crash (ASan/UBSan watch).
bool consistent(const DxilReadResult &result) {
    if (!result.error.empty()) {
        return result.info.bindings.empty() && !result.info.hasRootData && result.info.rootDataSize == 0;
    }
    return result.info.hasRootData || result.info.rootDataSize == 0;
}

} // namespace

// fill.hlsl: ResourceDescriptorHeap[] and a 20-byte FillRoot.
EQ_TEST(dxil, fill_heap_and_root_data) {
    const DxilReadResult result = read(loadShader("fill"));
    EQ_CHECK(result.error.empty());
    EQ_CHECK(result.info.resourceHeapIndexing);
    EQ_CHECK(!result.info.samplerHeapIndexing);
    EQ_CHECK(result.info.hasRootData);
    EQ_CHECK(result.info.rootDataSize == 20);
    EQ_CHECK(result.info.bindings.empty());
    EQ_CHECK(threadGroupIs(result, 64, 1, 1));
}

// The 64-bit atomics bits (P105), as the pinned DXC sets them: atomic64.hlsl
// uses them on a heap buffer and on groupshared memory; fill.hlsl uses none.
EQ_TEST(dxil, int64_atomics) {
    const DxilReadResult atomics = read(loadShader("atomic64"));
    EQ_CHECK(atomics.error.empty());
    EQ_CHECK(atomics.info.int64HeapAtomics && atomics.info.int64SharedAtomics);
    const DxilReadResult none = read(loadShader("fill"));
    EQ_CHECK(!none.info.int64HeapAtomics && !none.info.int64SharedAtomics);
}

EQ_TEST(dxil, root_data_4_bytes) {
    const DxilReadResult result = read(loadShader("root4"));
    EQ_CHECK(result.error.empty());
    EQ_CHECK(result.info.hasRootData);
    EQ_CHECK(result.info.rootDataSize == 4);
    EQ_CHECK(threadGroupIs(result, 8, 4, 2));
}

EQ_TEST(dxil, root_data_64_bytes) {
    const DxilReadResult result = read(loadShader("root64"));
    EQ_CHECK(result.error.empty());
    EQ_CHECK(result.info.hasRootData);
    EQ_CHECK(result.info.rootDataSize == 64);
    EQ_CHECK(result.info.resourceHeapIndexing);
}

// DX constant-buffer packing: each array element starts a 16-byte register, so
// the last member sits at 104 and the size is 108 (a C struct would be 36).
EQ_TEST(dxil, root_data_with_arrays) {
    const DxilReadResult result = read(loadShader("root_arrays"));
    EQ_CHECK(result.error.empty());
    EQ_CHECK(result.info.hasRootData);
    EQ_CHECK(result.info.rootDataSize == 108);
}

// No root data; declared CBV, sampler, unbounded SRV array, and UAV.
EQ_TEST(dxil, declared_bindings) {
    const DxilReadResult result = read(loadShader("declared"));
    EQ_CHECK(result.error.empty());
    EQ_CHECK(!result.info.hasRootData);
    EQ_CHECK(result.info.rootDataSize == 0);
    EQ_CHECK(!result.info.resourceHeapIndexing);
    EQ_CHECK(!result.info.samplerHeapIndexing);
    EQ_CHECK(result.info.bindings.size() == 4);
    EQ_CHECK(hasBinding(result, DxilResourceClass::eCBV, 0, 0, 1));
    EQ_CHECK(hasBinding(result, DxilResourceClass::eSampler, 0, 3, 1));
    EQ_CHECK(hasBinding(result, DxilResourceClass::eSRV, 2, 0, std::numeric_limits<uint32>::max()));
    EQ_CHECK(hasBinding(result, DxilResourceClass::eUAV, 1, 0, 1));
    EQ_CHECK(threadGroupIs(result, 64, 1, 1));
}

EQ_TEST(dxil, sampler_heap_only) {
    const DxilReadResult result = read(loadShader("sampler_heap"));
    EQ_CHECK(result.error.empty());
    EQ_CHECK(result.info.samplerHeapIndexing);
    EQ_CHECK(!result.info.resourceHeapIndexing);
    EQ_CHECK(result.info.hasRootData);
    EQ_CHECK(result.info.rootDataSize == 4);
    EQ_CHECK(result.info.bindings.size() == 2);
    EQ_CHECK(hasBinding(result, DxilResourceClass::eSRV, 0, 0, 1));
    EQ_CHECK(hasBinding(result, DxilResourceClass::eUAV, 0, 0, 1));
}

EQ_TEST(dxil, space999_other_register_is_error) {
    const DxilReadResult result = read(loadShader("misuse_b1"));
    EQ_CHECK(mentions(result, "space 999 is reserved for EQ_ROOT_DATA"));
    EQ_CHECK(mentions(result, "register(b1, space999)"));
    EQ_CHECK(!result.info.hasRootData);
}

EQ_TEST(dxil, space999_second_resource_is_error) {
    const DxilReadResult result = read(loadShader("misuse_extra"));
    EQ_CHECK(mentions(result, "space 999 is reserved for EQ_ROOT_DATA"));
    EQ_CHECK(mentions(result, "register(u0, space999)"));
}

// PSV0 is versioned: later versions append fields to the runtime info and to
// each resource record, and announce their sizes.
EQ_TEST(dxil, newer_psv_layout) {
    std::vector<Part> parts = splitParts(loadShader("declared"));
    Part *psv = findPart(parts, fourCC("PSV0"));
    EQ_CHECK(psv != nullptr);
    if (psv == nullptr) {
        return;
    }
    psv->data = resizePsv(psv->data, loadU32(psv->data, 0) + 12, 40);
    const DxilReadResult result = read(joinParts(parts));
    EQ_CHECK(result.error.empty());
    EQ_CHECK(result.info.bindings.size() == 4);
    EQ_CHECK(hasBinding(result, DxilResourceClass::eSRV, 2, 0, std::numeric_limits<uint32>::max()));
    EQ_CHECK(threadGroupIs(result, 64, 1, 1));
}

// A version-0 PSV0 (24-byte runtime info, 16-byte records) has no thread-group
// size; it then comes from the dx.entryPoints metadata.
EQ_TEST(dxil, oldest_psv_layout_numthreads_from_metadata) {
    std::vector<Part> parts = splitParts(loadShader("root4"));
    Part *psv = findPart(parts, fourCC("PSV0"));
    EQ_CHECK(psv != nullptr);
    if (psv == nullptr) {
        return;
    }
    psv->data = resizePsv(psv->data, 24, 16);
    const DxilReadResult result = read(joinParts(parts));
    EQ_CHECK(result.error.empty());
    EQ_CHECK(result.info.rootDataSize == 4);
    EQ_CHECK(threadGroupIs(result, 8, 4, 2));
}

EQ_TEST(dxil, empty_input) {
    const DxilReadResult result = readDxil({});
    EQ_CHECK(mentions(result, "too small"));
}

EQ_TEST(dxil, not_a_container) {
    Bytes bytes = loadShader("fill");
    bytes[0] = std::byte{'X'};
    EQ_CHECK(mentions(read(bytes), "DXBC"));
}

// Every prefix of a valid container is rejected (the header's size no longer fits).
EQ_TEST(dxil, truncated_container) {
    const Bytes full = loadShader("fill");
    for (size_t size = 0; size < full.size(); ++size) {
        const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(size));
        EQ_CHECK(!read(cut).error.empty());
    }
}

EQ_TEST(dxil, missing_parts) {
    for (const uint32 code : {fourCC("SFI0"), fourCC("PSV0"), fourCC("DXIL")}) {
        std::vector<Part> parts = splitParts(loadShader("fill"));
        std::erase_if(parts, [code](const Part &part) { return part.code == code; });
        EQ_CHECK(mentions(read(joinParts(parts)), "has no"));
    }
}

// The DXIL program header claims less bitcode than there is: every shorter
// size cuts the module block and is an error.
EQ_TEST(dxil, truncated_bitcode) {
    std::vector<Part> parts = splitParts(loadShader("fill"));
    Part *dxil = findPart(parts, fourCC("DXIL"));
    EQ_CHECK(dxil != nullptr);
    if (dxil == nullptr) {
        return;
    }
    const uint32 fullSize = loadU32(dxil->data, 20);
    for (uint32 size = 0; size < fullSize; ++size) {
        storeU32(dxil->data, 20, size);
        EQ_CHECK(!read(joinParts(parts)).error.empty());
    }
}

// Flips bits throughout the bitcode of several shaders. Most corruptions are
// caught; the rest decode to something else. Neither may crash.
EQ_TEST(dxil, corrupted_bitcode) {
    for (const char *name : {"fill", "declared", "root_arrays"}) {
        const Bytes original = loadShader(name);
        std::vector<Part> parts = splitParts(original);
        const Part *dxil = findPart(parts, fourCC("DXIL"));
        EQ_CHECK(dxil != nullptr);
        if (dxil == nullptr) {
            continue;
        }
        const size_t begin = dxil->offset + 8 + loadU32(dxil->data, 16); // first bitcode byte
        size_t errors = 0;
        size_t reads = 0;
        for (size_t offset = begin; offset < dxil->offset + dxil->data.size(); ++offset) {
            for (const uint8 mask : {uint8{0x01}, uint8{0x80}, uint8{0xFF}}) {
                Bytes corrupted = original;
                corrupted[offset] ^= std::byte{mask};
                const DxilReadResult result = read(corrupted);
                EQ_CHECK(consistent(result));
                errors += result.error.empty() ? 0 : 1;
                ++reads;
            }
        }
        EQ_CHECK(errors > 0 && errors < reads);
    }
}

// Corrupts every byte of a whole container (header, part table, PSV0, ...).
EQ_TEST(dxil, corrupted_container) {
    const Bytes original = loadShader("declared");
    for (size_t offset = 0; offset < original.size(); ++offset) {
        for (const uint8 mask : {uint8{0x04}, uint8{0xFF}}) {
            Bytes corrupted = original;
            corrupted[offset] ^= std::byte{mask};
            EQ_CHECK(consistent(read(corrupted)));
        }
    }
}

// Random garbage after a valid container header and inside a forged DXIL part.
EQ_TEST(dxil, garbage_bitcode) {
    std::vector<Part> parts = splitParts(loadShader("fill"));
    Part *dxil = findPart(parts, fourCC("DXIL"));
    EQ_CHECK(dxil != nullptr);
    if (dxil == nullptr) {
        return;
    }
    const size_t begin = 8 + loadU32(dxil->data, 16) + 4; // keep the 'BC' 0xC0DE magic
    uint32 state = 12345;
    for (int round = 0; round < 200; ++round) {
        for (size_t i = begin; i < dxil->data.size(); ++i) {
            state = state * 1664525u + 1013904223u;
            dxil->data[i] = static_cast<std::byte>(state >> 24);
        }
        EQ_CHECK(!read(joinParts(parts)).error.empty());
    }
}

// link_vs.hlsl and link_skip_ps.hlsl: the signatures, as the stage link check
// reads them (P101); they survive -Qstrip_reflect.
EQ_TEST(dxil, signatures) {
    const DxilReadResult vs = read(loadShader("link_vs"));
    EQ_CHECK(vs.error.empty());
    EQ_CHECK(vs.info.inputs.size() == 1 && vs.info.inputs[0].semanticName == "SV_VertexID");
    EQ_CHECK(vs.info.outputs.size() == 4);
    if (vs.info.outputs.size() == 4) {
        EQ_CHECK(vs.info.outputs[0].semanticName == "SV_Position");
        EQ_CHECK(vs.info.outputs[0].systemValue == RI::Reflection::kDxilSystemValuePosition);
        EQ_CHECK(vs.info.outputs[0].mask == 0xF);
        EQ_CHECK(vs.info.outputs[2].semanticName == "NORMAL" && vs.info.outputs[2].registerIndex == 2);
        EQ_CHECK(vs.info.outputs[3].semanticName == "COLOR" && vs.info.outputs[3].semanticIndex == 1);
        EQ_CHECK(vs.info.outputs[3].systemValue == RI::Reflection::kDxilSystemValueNone);
    }
    EQ_CHECK(vs.info.primitiveOutputs.empty());

    const DxilReadResult ps = read(loadShader("link_skip_ps"));
    EQ_CHECK(ps.error.empty());
    EQ_CHECK(ps.info.inputs.size() == 2);
    if (ps.info.inputs.size() == 2) {
        EQ_CHECK(ps.info.inputs[0].semanticName == "NORMAL" && ps.info.inputs[0].registerIndex == 0);
        EQ_CHECK(ps.info.inputs[1].semanticName == "SV_IsFrontFace");
        EQ_CHECK(ps.info.inputs[1].systemValue != RI::Reflection::kDxilSystemValueNone);
    }

    // Malformed signatures are errors: too many elements, a name outside the
    // part, a name with no terminator.
    const Bytes original = loadShader("link_vs");
    const auto corrupt = [&](size_t offset, uint32 value) {
        std::vector<Part> parts = splitParts(original);
        Part *osg1 = findPart(parts, fourCC("OSG1"));
        EQ_CHECK(osg1 != nullptr);
        storeU32(osg1->data, offset, value);
        return read(joinParts(parts));
    };
    EQ_CHECK(mentions(corrupt(0, 1000), "the OSG1 part's 1000 elements do not fit"));
    EQ_CHECK(mentions(corrupt(8 + 4, 0xFFFF), "semantic name lies outside the part"));
    std::vector<Part> parts = splitParts(original);
    Part *osg1 = findPart(parts, fourCC("OSG1"));
    EQ_CHECK(osg1 != nullptr);
    storeU32(osg1->data, 8 + 4, static_cast<uint32>(osg1->data.size() - 1));
    osg1->data.back() = std::byte{'X'};
    EQ_CHECK(mentions(read(joinParts(parts)), "semantic name is not terminated"));
}

// Hull, domain and geometry shaders read like other stages (P102): root data,
// and the outputs the pixel shader is checked against.
EQ_TEST(dxil, pipeline_stages) {
    const DxilReadResult hs = read(loadShader("patch_hs"));
    EQ_CHECK(hs.error.empty() && !hs.info.hasRootData);
    const DxilReadResult ds = read(loadShader("patch_ds"));
    EQ_CHECK(ds.error.empty() && ds.info.hasRootData && ds.info.rootDataSize == 20);
    EQ_CHECK(ds.info.outputs.size() == 2);
    if (ds.info.outputs.size() == 2) {
        EQ_CHECK(ds.info.outputs[1].semanticName == "COLOR" && ds.info.outputs[1].registerIndex == 1);
    }
    const DxilReadResult gs = read(loadShader("layered_gs"));
    EQ_CHECK(gs.error.empty() && !gs.info.hasRootData);
    EQ_CHECK(gs.info.outputs.size() == 3);
    if (gs.info.outputs.size() == 3) {
        EQ_CHECK(gs.info.outputs[1].semanticName == "COLOR" && gs.info.outputs[1].registerIndex == 1);
        EQ_CHECK(gs.info.outputs[2].systemValue != RI::Reflection::kDxilSystemValueNone);
    }
}
