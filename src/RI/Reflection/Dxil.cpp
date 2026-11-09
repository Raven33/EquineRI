// Implements Dxil.hpp: RI's own DXIL reader (P67). Parses the container parts
// (SFI0, PSV0, the signatures) and the LLVM 3.7 bitcode of the DXIL part far enough to read the
// root data size from `!dx.resources`. Every read is bounds-checked: malformed
// input gives an error message, never a crash.

#include <RI/Reflection/Dxil.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace RI::Reflection {

namespace {

// ── Little-endian access ─────────────────────────────────────────────────────
// Callers check bounds first; these only assemble bytes.

uint32 loadU32(std::span<const std::byte> bytes, uint64 offset) {
    uint32 value = 0;
    for (uint32 i = 0; i < 4; ++i) {
        value |= std::to_integer<uint32>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

uint64 loadU64(std::span<const std::byte> bytes, uint64 offset) {
    return uint64{loadU32(bytes, offset)} | (uint64{loadU32(bytes, offset + 4)} << 32);
}

constexpr uint32 fourCC(char a, char b, char c, char d) {
    return uint32(uint8(a)) | (uint32(uint8(b)) << 8) | (uint32(uint8(c)) << 16) | (uint32(uint8(d)) << 24);
}

constexpr uint32 kPartSFI0 = fourCC('S', 'F', 'I', '0');
constexpr uint32 kPartPSV0 = fourCC('P', 'S', 'V', '0');
constexpr uint32 kPartDXIL = fourCC('D', 'X', 'I', 'L');
constexpr uint32 kPartISG1 = fourCC('I', 'S', 'G', '1');
constexpr uint32 kPartOSG1 = fourCC('O', 'S', 'G', '1');
constexpr uint32 kPartPSG1 = fourCC('P', 'S', 'G', '1');

// ── DXBC container ───────────────────────────────────────────────────────────
// Header: "DXBC", 16-byte hash, u16 major + u16 minor version, u32 total size,
// u32 part count, then one u32 offset per part. Each part: fourcc, u32 size, data.

constexpr uint64 kContainerHeaderSize = 32;
constexpr uint64 kPartHeaderSize = 8;

struct Parts {
    std::span<const std::byte> sfi0;
    std::span<const std::byte> psv0;
    std::span<const std::byte> dxil;
    std::span<const std::byte> isg1;
    std::span<const std::byte> osg1;
    std::span<const std::byte> psg1;
    bool hasSfi0 = false;
    bool hasPsv0 = false;
    bool hasDxil = false;
    bool hasIsg1 = false;
    bool hasOsg1 = false;
    bool hasPsg1 = false;
};

std::string findParts(std::span<const std::byte> bytes, Parts &parts) {
    if (bytes.size() < kContainerHeaderSize) {
        return std::format("the bytecode is too small for a DXBC container ({} bytes)", bytes.size());
    }
    if (loadU32(bytes, 0) != fourCC('D', 'X', 'B', 'C')) {
        return "the bytecode is not a DXBC container (missing 'DXBC' magic)";
    }
    const uint32 majorVersion = loadU32(bytes, 20) & 0xFFFFu;
    if (majorVersion != 1) {
        return std::format("unsupported DXBC container version {}", majorVersion);
    }
    const uint64 containerSize = loadU32(bytes, 24);
    if (containerSize < kContainerHeaderSize || containerSize > bytes.size()) {
        return std::format("the DXBC container claims {} bytes but {} were given", containerSize, bytes.size());
    }
    bytes = bytes.first(containerSize);

    const uint64 partCount = loadU32(bytes, 28);
    if (partCount > (containerSize - kContainerHeaderSize) / 4) {
        return std::format("the DXBC container's part table ({} parts) does not fit in the container", partCount);
    }
    for (uint64 i = 0; i < partCount; ++i) {
        const uint64 offset = loadU32(bytes, kContainerHeaderSize + 4 * i);
        if (offset > containerSize - kPartHeaderSize) {
            return std::format("DXBC part {} starts outside the container", i);
        }
        const uint32 code = loadU32(bytes, offset);
        const uint64 size = loadU32(bytes, offset + 4);
        if (size > containerSize - offset - kPartHeaderSize) {
            return std::format("DXBC part {} ends outside the container", i);
        }
        const std::span<const std::byte> data = bytes.subspan(offset + kPartHeaderSize, size);

        std::span<const std::byte> *slot = nullptr;
        bool *seen = nullptr;
        std::string_view name;
        if (code == kPartSFI0) {
            slot = &parts.sfi0;
            seen = &parts.hasSfi0;
            name = "SFI0";
        } else if (code == kPartPSV0) {
            slot = &parts.psv0;
            seen = &parts.hasPsv0;
            name = "PSV0";
        } else if (code == kPartDXIL) {
            slot = &parts.dxil;
            seen = &parts.hasDxil;
            name = "DXIL";
        } else if (code == kPartISG1) {
            slot = &parts.isg1;
            seen = &parts.hasIsg1;
            name = "ISG1";
        } else if (code == kPartOSG1) {
            slot = &parts.osg1;
            seen = &parts.hasOsg1;
            name = "OSG1";
        } else if (code == kPartPSG1) {
            slot = &parts.psg1;
            seen = &parts.hasPsg1;
            name = "PSG1";
        } else {
            continue;
        }
        if (*seen) {
            return std::format("the DXBC container has two {} parts", name);
        }
        *slot = data;
        *seen = true;
    }
    if (!parts.hasDxil) {
        return "the DXBC container has no DXIL part";
    }
    if (!parts.hasPsv0) {
        return "the DXBC container has no PSV0 part (pipeline state validation)";
    }
    if (!parts.hasSfi0) {
        return "the DXBC container has no SFI0 part (shader feature flags)";
    }
    return {};
}

// ── PSV0: pipeline state validation ──────────────────────────────────────────
// u32 runtime-info size, the runtime info (versioned: it only grows), u32
// resource count, and when non-zero, u32 record size followed by the records
// (also versioned). Only the leading fields of each are read.

// PSVRuntimeInfo1: ShaderStage byte. PSVRuntimeInfo2: NumThreadsX/Y/Z.
constexpr uint64 kPsvStageOffset = 24;
constexpr uint64 kPsvNumThreadsOffset = 36;
constexpr uint64 kPsvRuntimeInfo2Size = 48;
// PSVShaderKind (DxilPipelineStateValidation.h): Compute 5, Mesh 13,
// Amplification 14 (verified with DXC 1.9.2609; only Metal reads the mesh and
// amplification sizes, P114 (2)).
constexpr uint8 kPsvStageCompute = 5;
constexpr uint8 kPsvStageMesh = 13;
constexpr uint8 kPsvStageAmplification = 14;

// PSVResourceBindInfo0: ResType, Space, LowerBound, UpperBound.
constexpr uint64 kPsvResourceRecordMinSize = 16;

struct PsvResource {
    uint32 type = 0;
    DxilBinding binding;
};

// PSVResourceType: Sampler=1, CBV=2, SRV Typed/Raw/Structured=3..5,
// UAV Typed/Raw/Structured/StructuredWithCounter=6..9.
std::optional<DxilResourceClass> psvResourceClass(uint32 type) {
    if (type == 1) {
        return DxilResourceClass::eSampler;
    }
    if (type == 2) {
        return DxilResourceClass::eCBV;
    }
    if (type >= 3 && type <= 5) {
        return DxilResourceClass::eSRV;
    }
    if (type >= 6 && type <= 9) {
        return DxilResourceClass::eUAV;
    }
    return std::nullopt;
}

std::string readPsv(std::span<const std::byte> part, DxilInfo &info, std::vector<DxilBinding> &resources) {
    const uint64 size = part.size();
    if (size < 4) {
        return "the PSV0 part is truncated";
    }
    const uint64 runtimeInfoSize = loadU32(part, 0);
    if (runtimeInfoSize > size - 4) {
        return "the PSV0 runtime info does not fit in its part";
    }
    const std::span<const std::byte> runtimeInfo = part.subspan(4, runtimeInfoSize);
    if (runtimeInfoSize >= kPsvRuntimeInfo2Size) {
        const auto stage = std::to_integer<uint8>(runtimeInfo[kPsvStageOffset]);
        if (stage == kPsvStageCompute || stage == kPsvStageAmplification || stage == kPsvStageMesh) {
            for (uint32 axis = 0; axis < 3; ++axis) {
                info.threadGroupSize[axis] = loadU32(runtimeInfo, kPsvNumThreadsOffset + 4 * axis);
            }
        }
    }

    uint64 offset = 4 + runtimeInfoSize;
    if (size - offset < 4) {
        return "the PSV0 part has no resource count";
    }
    const uint64 count = loadU32(part, offset);
    offset += 4;
    if (count == 0) {
        return {};
    }
    if (size - offset < 4) {
        return "the PSV0 part has no resource record size";
    }
    const uint64 recordSize = loadU32(part, offset);
    offset += 4;
    if (recordSize < kPsvResourceRecordMinSize) {
        return std::format("the PSV0 resource record size {} is smaller than the minimum {}", recordSize,
                           kPsvResourceRecordMinSize);
    }
    if (count > (size - offset) / recordSize) {
        return std::format("the PSV0 part's {} resource records do not fit in it", count);
    }
    for (uint64 i = 0; i < count; ++i, offset += recordSize) {
        const uint32 type = loadU32(part, offset);
        const std::optional<DxilResourceClass> resourceClass = psvResourceClass(type);
        if (!resourceClass) {
            return std::format("the PSV0 part has a resource of unknown type {}", type);
        }
        DxilBinding binding;
        binding.resourceClass = *resourceClass;
        binding.space = loadU32(part, offset + 4);
        binding.lowerBound = loadU32(part, offset + 8);
        const uint32 upperBound = loadU32(part, offset + 12);
        if (upperBound == std::numeric_limits<uint32>::max()) {
            binding.count = std::numeric_limits<uint32>::max(); // unbounded array
        } else if (upperBound < binding.lowerBound) {
            return std::format("a PSV0 resource range ends ({}) before it starts ({})", upperBound, binding.lowerBound);
        } else {
            binding.count = upperBound - binding.lowerBound + 1;
        }
        resources.push_back(binding);
    }
    return {};
}

// ── LLVM bitstream reader ────────────────────────────────────────────────────
// Bits are read least-significant first from consecutive bytes. Failures are
// sticky: the first one is kept in `error`, later reads return 0, and every loop
// checks for failure, so malformed input ends parsing instead of crashing.

struct BitReader {
    std::span<const std::byte> bytes;
    uint64 bit = 0;
    std::string error;
};

bool failed(const BitReader &reader) { return !reader.error.empty(); }

void fail(BitReader &reader, std::string message) {
    if (reader.error.empty()) {
        reader.error = std::move(message);
    }
}

uint64 totalBits(const BitReader &reader) { return uint64{reader.bytes.size()} * 8; }

uint64 remainingBits(const BitReader &reader) {
    return failed(reader) || reader.bit > totalBits(reader) ? 0 : totalBits(reader) - reader.bit;
}

uint64 readFixed(BitReader &reader, uint32 width) {
    if (failed(reader)) {
        return 0;
    }
    if (width > 64 || width > remainingBits(reader)) {
        fail(reader, "unexpected end of bitcode");
        return 0;
    }
    uint64 value = 0;
    uint32 done = 0;
    while (done < width) {
        const uint32 bitInByte = reader.bit % 8;
        const uint32 take = std::min(8 - bitInByte, width - done);
        const uint64 byte = std::to_integer<uint64>(reader.bytes[reader.bit / 8]);
        value |= ((byte >> bitInByte) & ((uint64{1} << take) - 1)) << done;
        done += take;
        reader.bit += take;
    }
    return value;
}

// Variable-width integer: chunks of `width` bits, the top bit of each chunk
// says another chunk follows. `width` is in [2, 32].
uint64 readVbr(BitReader &reader, uint32 width) {
    const uint64 continueBit = uint64{1} << (width - 1);
    uint64 value = 0;
    uint32 shift = 0;
    while (!failed(reader)) {
        const uint64 chunk = readFixed(reader, width);
        const uint64 payload = chunk & (continueBit - 1);
        if (shift >= 64 || (shift > 0 && (payload >> (64 - shift)) != 0)) {
            fail(reader, "a variable-width integer does not fit in 64 bits");
            break;
        }
        value |= payload << shift;
        if ((chunk & continueBit) == 0) {
            return value;
        }
        shift += width - 1;
    }
    return 0;
}

void alignTo32(BitReader &reader) {
    const uint64 aligned = (reader.bit + 31) & ~uint64{31};
    if (aligned > totalBits(reader)) {
        fail(reader, "unexpected end of bitcode");
        return;
    }
    reader.bit = aligned;
}

// LLVM's signed VBR: the sign is in bit 0.
int64 decodeSigned(uint64 value) {
    if ((value & 1) == 0) {
        return static_cast<int64>(value >> 1);
    }
    if (value != 1) {
        return -static_cast<int64>(value >> 1);
    }
    return std::numeric_limits<int64>::min();
}

// ── Abbreviations and records ────────────────────────────────────────────────

enum class OpKind : uint8 {
    eLiteral,
    eFixed,
    eVbr,
    eArray,
    eChar6,
    eBlob,
};

struct AbbrevOp {
    OpKind kind = OpKind::eLiteral;
    uint64 value = 0; // literal value, or the width of eFixed / eVbr
};

struct Abbrev {
    std::vector<AbbrevOp> ops;
};

struct Record {
    uint64 code = 0;
    std::vector<uint64> ops;
};

// Fixed abbreviation ids, valid in every block.
constexpr uint64 kEndBlock = 0;
constexpr uint64 kEnterSubblock = 1;
constexpr uint64 kDefineAbbrev = 2;
constexpr uint64 kUnabbrevRecord = 3;
constexpr uint64 kFirstAbbrevId = 4;

void readAbbrevDefinition(BitReader &reader, Abbrev &abbrev) {
    abbrev.ops.clear();
    const uint64 count = readVbr(reader, 5);
    if (count == 0 || count > remainingBits(reader)) {
        fail(reader, "an abbreviation has an invalid operand count");
        return;
    }
    for (uint64 i = 0; i < count && !failed(reader); ++i) {
        if (readFixed(reader, 1) != 0) {
            abbrev.ops.push_back({OpKind::eLiteral, readVbr(reader, 8)});
            continue;
        }
        const uint64 encoding = readFixed(reader, 3);
        if (encoding == 1 || encoding == 2) {
            const uint64 width = readVbr(reader, 5);
            if (width == 0) {
                abbrev.ops.push_back({OpKind::eLiteral, 0}); // a zero-width field always reads 0 (LLVM)
            } else if (encoding == 1 && width <= 64) {
                abbrev.ops.push_back({OpKind::eFixed, width});
            } else if (encoding == 2 && width >= 2 && width <= 32) {
                abbrev.ops.push_back({OpKind::eVbr, width});
            } else {
                fail(reader, std::format("an abbreviation has an invalid field width {}", width));
            }
        } else if (encoding == 3) {
            abbrev.ops.push_back({OpKind::eArray, 0});
        } else if (encoding == 4) {
            abbrev.ops.push_back({OpKind::eChar6, 0});
        } else if (encoding == 5) {
            abbrev.ops.push_back({OpKind::eBlob, 0});
        } else {
            fail(reader, std::format("an abbreviation has an unknown encoding {}", encoding));
        }
    }
    if (failed(reader)) {
        return;
    }
    // Shape rules: the code comes first and is a scalar; an array is second to
    // last and its element (the last op) is a non-literal scalar; a blob is last.
    const size_t last = abbrev.ops.size() - 1;
    for (size_t i = 0; i < abbrev.ops.size(); ++i) {
        const OpKind kind = abbrev.ops[i].kind;
        const bool isAggregate = kind == OpKind::eArray || kind == OpKind::eBlob;
        if (i == 0 && isAggregate) {
            fail(reader, "an abbreviation starts with an array or a blob");
        } else if (kind == OpKind::eArray) {
            const OpKind element = i + 1 == last ? abbrev.ops[last].kind : OpKind::eArray;
            if (element != OpKind::eFixed && element != OpKind::eVbr && element != OpKind::eChar6) {
                fail(reader, "an abbreviation has a misplaced or invalid array");
            }
            break;
        } else if (kind == OpKind::eBlob && i != last) {
            fail(reader, "an abbreviation has a blob that is not its last operand");
        }
    }
}

uint64 readScalar(BitReader &reader, const AbbrevOp &op) {
    switch (op.kind) {
    case OpKind::eLiteral:
        return op.value;
    case OpKind::eFixed:
        return readFixed(reader, static_cast<uint32>(op.value));
    case OpKind::eVbr:
        return readVbr(reader, static_cast<uint32>(op.value));
    case OpKind::eChar6: {
        constexpr std::string_view kChar6 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
        return static_cast<uint8>(kChar6[readFixed(reader, 6)]);
    }
    case OpKind::eArray:
    case OpKind::eBlob:
        break;
    }
    fail(reader, "an aggregate abbreviation operand was used as a scalar");
    return 0;
}

void readAbbreviatedRecord(BitReader &reader, const Abbrev &abbrev, Record &record) {
    record.ops.clear();
    record.code = readScalar(reader, abbrev.ops[0]);
    for (size_t i = 1; i < abbrev.ops.size() && !failed(reader); ++i) {
        const AbbrevOp &op = abbrev.ops[i];
        if (op.kind == OpKind::eArray) {
            // Every element takes at least one bit, which bounds the count.
            const uint64 count = readVbr(reader, 6);
            if (count > remainingBits(reader)) {
                fail(reader, "an array operand is longer than the bitcode");
                return;
            }
            record.ops.reserve(record.ops.size() + count);
            for (uint64 e = 0; e < count && !failed(reader); ++e) {
                record.ops.push_back(readScalar(reader, abbrev.ops[i + 1]));
            }
            return; // the element op was the last one
        }
        if (op.kind == OpKind::eBlob) {
            const uint64 length = readVbr(reader, 6);
            alignTo32(reader);
            if (length > remainingBits(reader) / 8) {
                fail(reader, "a blob operand is longer than the bitcode");
                return;
            }
            record.ops.reserve(record.ops.size() + length);
            for (uint64 b = 0; b < length; ++b) {
                record.ops.push_back(std::to_integer<uint64>(reader.bytes[(reader.bit / 8) + b]));
            }
            reader.bit += length * 8;
            alignTo32(reader);
            return;
        }
        record.ops.push_back(readScalar(reader, op));
    }
}

void readUnabbreviatedRecord(BitReader &reader, Record &record) {
    record.ops.clear();
    record.code = readVbr(reader, 6);
    const uint64 count = readVbr(reader, 6);
    if (count > remainingBits(reader) / 6) {
        fail(reader, "a record has more operands than the bitcode holds");
        return;
    }
    record.ops.reserve(count);
    for (uint64 i = 0; i < count && !failed(reader); ++i) {
        record.ops.push_back(readVbr(reader, 6));
    }
}

// ── Blocks ───────────────────────────────────────────────────────────────────

struct Block {
    uint64 id = 0;
    uint32 abbrevWidth = 2;
    uint64 endBit = 0;           // first bit after the block
    std::vector<Abbrev> abbrevs; // BLOCKINFO abbreviations first, then the block's own
};

struct BlockInfo {
    uint64 blockId = 0;
    std::vector<Abbrev> abbrevs;
};

struct Bitcode {
    BitReader reader;
    std::vector<BlockInfo> blockInfos;
};

// Reads the rest of an ENTER_SUBBLOCK: id, abbreviation width, alignment, and
// the block's length in 32-bit words, which must stay inside the parent.
void enterBlock(Bitcode &bitcode, Block &block, uint64 parentEndBit) {
    BitReader &reader = bitcode.reader;
    block.id = readVbr(reader, 8);
    const uint64 width = readVbr(reader, 4);
    alignTo32(reader);
    const uint64 words = readFixed(reader, 32);
    if (failed(reader)) {
        return;
    }
    if (width == 0 || width > 32) {
        fail(reader, std::format("block {} has an invalid abbreviation width {}", block.id, width));
        return;
    }
    if (reader.bit > parentEndBit || words * 32 > parentEndBit - reader.bit) {
        fail(reader, std::format("block {} is longer than its parent", block.id));
        return;
    }
    block.abbrevWidth = static_cast<uint32>(width);
    block.endBit = reader.bit + words * 32;
    block.abbrevs.clear();
    for (const BlockInfo &info : bitcode.blockInfos) {
        if (info.blockId == block.id) {
            block.abbrevs = info.abbrevs;
        }
    }
}

enum class EntryKind : uint8 {
    eEnd,
    eSubblock,
    eRecord,
    eAbbrev,
    eError,
};

// Reads one entry of `block`. A record lands in `record`, an abbreviation
// definition in `abbrev` (the caller decides where it belongs), and an entered
// sub-block's header in `subblock`.
EntryKind readEntry(Bitcode &bitcode, const Block &block, Record &record, Abbrev &abbrev, Block &subblock) {
    BitReader &reader = bitcode.reader;
    if (reader.bit >= block.endBit) {
        fail(reader, std::format("block {} ends without END_BLOCK", block.id));
        return EntryKind::eError;
    }
    const uint64 id = readFixed(reader, block.abbrevWidth);
    EntryKind kind = EntryKind::eRecord;
    if (id == kEndBlock) {
        alignTo32(reader);
        if (!failed(reader) && reader.bit != block.endBit) {
            fail(reader, std::format("block {} does not end where its length says", block.id));
        }
        kind = EntryKind::eEnd;
    } else if (id == kEnterSubblock) {
        enterBlock(bitcode, subblock, block.endBit);
        kind = EntryKind::eSubblock;
    } else if (id == kDefineAbbrev) {
        readAbbrevDefinition(reader, abbrev);
        kind = EntryKind::eAbbrev;
    } else if (id == kUnabbrevRecord) {
        readUnabbreviatedRecord(reader, record);
    } else if (id - kFirstAbbrevId < block.abbrevs.size()) {
        readAbbreviatedRecord(reader, block.abbrevs[id - kFirstAbbrevId], record);
    } else {
        fail(reader, std::format("block {} uses undefined abbreviation {}", block.id, id));
    }
    if (!failed(reader) && reader.bit > block.endBit) {
        fail(reader, std::format("an entry runs past the end of block {}", block.id));
    }
    return failed(reader) ? EntryKind::eError : kind;
}

// ── LLVM 3.7 module contents ─────────────────────────────────────────────────

constexpr uint64 kBlockInfoBlock = 0;
constexpr uint64 kModuleBlock = 8;
constexpr uint64 kConstantsBlock = 11;
constexpr uint64 kMetadataBlock = 15;

constexpr uint64 kBlockInfoSetBid = 1;

// MODULE records that define a value id, in record order.
constexpr uint64 kModuleGlobalVar = 7;
constexpr uint64 kModuleFunction = 8;
constexpr uint64 kModuleAliasOld = 9;
constexpr uint64 kModuleAlias = 14;

// CONSTANTS records: every record but SETTYPE defines the next value id. Zero
// integers are written as NULL.
constexpr uint64 kConstSetType = 1;
constexpr uint64 kConstNull = 2;
constexpr uint64 kConstInteger = 4;

// METADATA records: every record but NAME, KIND, and NAMED_NODE defines the
// next metadata id. NODE operands are id + 1 (0 is null); NAMED_NODE operands
// are plain ids; VALUE is [type, absolute value id].
constexpr uint64 kMetadataString = 1;
constexpr uint64 kMetadataValue = 2;
constexpr uint64 kMetadataNode = 3;
constexpr uint64 kMetadataName = 4;
constexpr uint64 kMetadataDistinctNode = 5;
constexpr uint64 kMetadataKind = 6;
constexpr uint64 kMetadataNamedNode = 10;

enum class MetadataKind : uint8 {
    eOther,
    eString,
    eValue,
    eNode,
};

struct Metadata {
    MetadataKind kind = MetadataKind::eOther;
    uint64 valueId = 0;           // eValue
    std::vector<uint64> operands; // eNode: metadata id + 1, 0 for null
};

struct Module {
    bool found = false;
    uint64 valueCount = 0;
    std::unordered_map<uint64, int64> integers; // value id -> module-level integer constant
    std::vector<Metadata> metadata;
    std::unordered_map<std::string, std::vector<uint64>> namedMetadata;
};

void skipBlock(Bitcode &bitcode, const Block &block) { bitcode.reader.bit = block.endBit; }

void parseBlockInfo(Bitcode &bitcode, const Block &block) {
    Record record;
    Abbrev abbrev;
    Block subblock;
    std::optional<size_t> current;
    while (true) {
        const EntryKind kind = readEntry(bitcode, block, record, abbrev, subblock);
        if (kind == EntryKind::eEnd || kind == EntryKind::eError) {
            return;
        }
        if (kind == EntryKind::eSubblock) {
            skipBlock(bitcode, subblock);
        } else if (kind == EntryKind::eAbbrev) {
            if (!current) {
                fail(bitcode.reader, "BLOCKINFO defines an abbreviation before SETBID");
                return;
            }
            bitcode.blockInfos[*current].abbrevs.push_back(abbrev);
        } else if (record.code == kBlockInfoSetBid) {
            if (record.ops.empty()) {
                fail(bitcode.reader, "BLOCKINFO SETBID has no block id");
                return;
            }
            current.reset();
            for (size_t i = 0; i < bitcode.blockInfos.size(); ++i) {
                if (bitcode.blockInfos[i].blockId == record.ops[0]) {
                    current = i;
                }
            }
            if (!current) {
                bitcode.blockInfos.push_back({record.ops[0], {}});
                current = bitcode.blockInfos.size() - 1;
            }
        }
    }
}

void parseConstants(Bitcode &bitcode, Block &block, Module &module) {
    Record record;
    Abbrev abbrev;
    Block subblock;
    while (true) {
        const EntryKind kind = readEntry(bitcode, block, record, abbrev, subblock);
        if (kind == EntryKind::eEnd || kind == EntryKind::eError) {
            return;
        }
        if (kind == EntryKind::eSubblock) {
            skipBlock(bitcode, subblock);
            continue;
        }
        if (kind == EntryKind::eAbbrev) {
            block.abbrevs.push_back(abbrev);
            continue;
        }
        if (record.code == kConstSetType) {
            continue;
        }
        if (record.code == kConstInteger) {
            if (record.ops.empty()) {
                fail(bitcode.reader, "an integer constant has no value");
                return;
            }
            module.integers[module.valueCount] = decodeSigned(record.ops[0]);
        } else if (record.code == kConstNull) {
            module.integers[module.valueCount] = 0;
        }
        ++module.valueCount;
    }
}

void parseMetadata(Bitcode &bitcode, Block &block, Module &module) {
    Record record;
    Abbrev abbrev;
    Block subblock;
    std::optional<std::string> pendingName; // METADATA_NAME waits for its NAMED_NODE
    while (true) {
        const EntryKind kind = readEntry(bitcode, block, record, abbrev, subblock);
        if (kind == EntryKind::eError) {
            return;
        }
        if (pendingName && (kind != EntryKind::eRecord || record.code != kMetadataNamedNode)) {
            fail(bitcode.reader, "METADATA_NAME is not followed by METADATA_NAMED_NODE");
            return;
        }
        if (kind == EntryKind::eEnd) {
            return;
        }
        if (kind == EntryKind::eSubblock) {
            skipBlock(bitcode, subblock);
            continue;
        }
        if (kind == EntryKind::eAbbrev) {
            block.abbrevs.push_back(abbrev);
            continue;
        }
        Metadata metadata;
        switch (record.code) {
        case kMetadataName: {
            std::string name;
            for (const uint64 c : record.ops) {
                name.push_back(static_cast<char>(c));
            }
            pendingName = std::move(name);
            continue;
        }
        case kMetadataNamedNode:
            if (!pendingName) {
                fail(bitcode.reader, "METADATA_NAMED_NODE without a METADATA_NAME");
                return;
            }
            module.namedMetadata[std::move(*pendingName)] = record.ops;
            pendingName.reset();
            continue;
        case kMetadataKind:
            continue;
        case kMetadataString:
            metadata.kind = MetadataKind::eString;
            break;
        case kMetadataValue:
            if (record.ops.size() != 2) {
                fail(bitcode.reader, "METADATA_VALUE does not have two operands");
                return;
            }
            metadata.kind = MetadataKind::eValue;
            metadata.valueId = record.ops[1];
            break;
        case kMetadataNode:
        case kMetadataDistinctNode:
            metadata.kind = MetadataKind::eNode;
            metadata.operands = record.ops;
            break;
        default:
            break; // debug info and others: only the id counts
        }
        module.metadata.push_back(std::move(metadata));
    }
}

void parseModule(Bitcode &bitcode, Block &block, Module &module) {
    module.found = true;
    Record record;
    Abbrev abbrev;
    Block subblock;
    while (true) {
        const EntryKind kind = readEntry(bitcode, block, record, abbrev, subblock);
        if (kind == EntryKind::eEnd || kind == EntryKind::eError) {
            return;
        }
        if (kind == EntryKind::eAbbrev) {
            block.abbrevs.push_back(abbrev);
        } else if (kind == EntryKind::eRecord) {
            if (record.code == kModuleGlobalVar || record.code == kModuleFunction ||
                record.code == kModuleAliasOld || record.code == kModuleAlias) {
                ++module.valueCount;
            }
        } else if (subblock.id == kBlockInfoBlock) {
            parseBlockInfo(bitcode, subblock);
        } else if (subblock.id == kConstantsBlock) {
            parseConstants(bitcode, subblock, module);
        } else if (subblock.id == kMetadataBlock) {
            parseMetadata(bitcode, subblock, module);
        } else {
            skipBlock(bitcode, subblock); // types, attributes, function bodies, symbol tables, ...
        }
    }
}

// Parses the bitcode up to the end of the module block. Returns an error message.
std::string parseBitcode(std::span<const std::byte> bytes, Module &module) {
    if (bytes.size() < 4 || std::to_integer<uint8>(bytes[0]) != 'B' || std::to_integer<uint8>(bytes[1]) != 'C' ||
        std::to_integer<uint8>(bytes[2]) != 0xC0 || std::to_integer<uint8>(bytes[3]) != 0xDE) {
        return "the DXIL part's bitcode does not start with the 'BC' 0xC0DE magic";
    }
    Bitcode bitcode;
    bitcode.reader.bytes = bytes;
    bitcode.reader.bit = 32;
    Block block;
    // The top level holds only blocks (abbreviation width 2) and may end with padding.
    while (!failed(bitcode.reader) && !module.found && remainingBits(bitcode.reader) >= 32) {
        if (readFixed(bitcode.reader, 2) != kEnterSubblock) {
            fail(bitcode.reader, "expected a block at the top level");
            break;
        }
        enterBlock(bitcode, block, totalBits(bitcode.reader));
        if (failed(bitcode.reader)) {
            break;
        }
        if (block.id == kModuleBlock) {
            parseModule(bitcode, block, module);
        } else if (block.id == kBlockInfoBlock) {
            parseBlockInfo(bitcode, block);
        } else {
            skipBlock(bitcode, block);
        }
    }
    if (failed(bitcode.reader)) {
        return "malformed DXIL bitcode: " + bitcode.reader.error;
    }
    if (!module.found) {
        return "malformed DXIL bitcode: no module block";
    }
    return {};
}

// ── Metadata queries ─────────────────────────────────────────────────────────

const Metadata *metadataAt(const Module &module, uint64 id) {
    return id < module.metadata.size() ? &module.metadata[id] : nullptr;
}

bool isNullOperand(const Metadata &node, size_t index) {
    return index < node.operands.size() && node.operands[index] == 0;
}

const Metadata *nodeOperand(const Module &module, const Metadata &node, size_t index) {
    if (index >= node.operands.size() || node.operands[index] == 0) {
        return nullptr;
    }
    const Metadata *operand = metadataAt(module, node.operands[index] - 1);
    return operand != nullptr && operand->kind == MetadataKind::eNode ? operand : nullptr;
}

std::optional<int64> integerOperand(const Module &module, const Metadata &node, size_t index) {
    if (index >= node.operands.size() || node.operands[index] == 0) {
        return std::nullopt;
    }
    const Metadata *operand = metadataAt(module, node.operands[index] - 1);
    if (operand == nullptr || operand->kind != MetadataKind::eValue) {
        return std::nullopt;
    }
    const auto it = module.integers.find(operand->valueId);
    if (it == module.integers.end()) {
        return std::nullopt;
    }
    return it->second;
}

// An i32 field: DXC writes values up to UINT32_MAX, which read back negative.
std::optional<uint32> uint32Operand(const Module &module, const Metadata &node, size_t index) {
    const std::optional<int64> value = integerOperand(module, node, index);
    if (!value || *value < std::numeric_limits<int32>::min() || *value > std::numeric_limits<uint32>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32>(*value);
}

// The one node a named metadata lists (`!dx.resources = !{!4}` gives !4).
const Metadata *namedNode(const Module &module, const std::string &name) {
    const auto it = module.namedMetadata.find(name);
    if (it == module.namedMetadata.end() || it->second.size() != 1) {
        return nullptr;
    }
    const Metadata *node = metadataAt(module, it->second[0]);
    return node != nullptr && node->kind == MetadataKind::eNode ? node : nullptr;
}

// dx.resources' node = !{SRVs, UAVs, CBVs, Samplers}, each null or a list of records.
// Every record starts with ID, global symbol, name, space, lower bound, range
// size; a CBV record continues with its size in bytes and an extra-fields node
// (DxilMetadataHelper: kDxilCBufferSizeInBytes = 6). Finds the root data CBV.
constexpr size_t kResourceListCBVs = 2;
constexpr size_t kResourceSpace = 3;
constexpr size_t kResourceLowerBound = 4;
constexpr size_t kResourceRangeSize = 5;
constexpr size_t kCBufferSizeInBytes = 6;

std::string findRootDataSize(const Module &module, std::optional<uint32> &rootDataSize) {
    if (!module.namedMetadata.contains("dx.resources")) {
        return {}; // a shader without declared resources has no list
    }
    const Metadata *resources = namedNode(module, "dx.resources");
    if (resources == nullptr || resources->operands.size() <= kResourceListCBVs) {
        return "malformed dx.resources metadata in the DXIL part";
    }
    if (isNullOperand(*resources, kResourceListCBVs)) {
        return {};
    }
    const Metadata *cbvs = nodeOperand(module, *resources, kResourceListCBVs);
    if (cbvs == nullptr) {
        return "malformed CBV list in the DXIL part's dx.resources metadata";
    }
    for (size_t i = 0; i < cbvs->operands.size(); ++i) {
        const Metadata *cbv = nodeOperand(module, *cbvs, i);
        if (cbv == nullptr) {
            return "malformed CBV record in the DXIL part's dx.resources metadata";
        }
        const std::optional<uint32> space = uint32Operand(module, *cbv, kResourceSpace);
        const std::optional<uint32> lowerBound = uint32Operand(module, *cbv, kResourceLowerBound);
        const std::optional<uint32> rangeSize = uint32Operand(module, *cbv, kResourceRangeSize);
        const std::optional<uint32> size = uint32Operand(module, *cbv, kCBufferSizeInBytes);
        if (!space || !lowerBound || !rangeSize || !size) {
            return "malformed CBV record in the DXIL part's dx.resources metadata";
        }
        if (*space == kRootDataSpace && *lowerBound == 0) {
            if (rootDataSize) {
                return "the DXIL part's dx.resources metadata lists two CBVs at register(b0, space999)";
            }
            rootDataSize = *size;
        }
    }
    return {};
}

// Fallback for the thread-group size when PSV0 predates it: a shader's single
// dx.entryPoints entry is !{function, name, signatures, resources, properties}; the
// properties are tag/value pairs and tag 4 (kDxilNumThreadsTag) holds !{x, y, z}.
void readNumThreadsMetadata(const Module &module, uint32 (&threadGroupSize)[3]) {
    const Metadata *entry = namedNode(module, "dx.entryPoints");
    const Metadata *properties = entry != nullptr ? nodeOperand(module, *entry, 4) : nullptr;
    if (properties == nullptr) {
        return;
    }
    constexpr int64 kNumThreadsTag = 4;
    for (size_t i = 0; i + 1 < properties->operands.size(); i += 2) {
        if (integerOperand(module, *properties, i) != kNumThreadsTag) {
            continue;
        }
        const Metadata *sizes = nodeOperand(module, *properties, i + 1);
        uint32 values[3] = {0, 0, 0};
        for (uint32 axis = 0; sizes != nullptr && axis < 3; ++axis) {
            const std::optional<uint32> value = uint32Operand(module, *sizes, axis);
            if (!value) {
                return;
            }
            values[axis] = *value;
        }
        if (sizes != nullptr) {
            for (uint32 axis = 0; axis < 3; ++axis) {
                threadGroupSize[axis] = values[axis];
            }
        }
        return;
    }
}

// ── Root data rules (P45) ────────────────────────────────────────────────────

char registerLetter(DxilResourceClass resourceClass) {
    switch (resourceClass) {
    case DxilResourceClass::eSRV:
        return 't';
    case DxilResourceClass::eUAV:
        return 'u';
    case DxilResourceClass::eCBV:
        return 'b';
    case DxilResourceClass::eSampler:
        return 's';
    }
    return '?';
}

std::string_view className(DxilResourceClass resourceClass) {
    switch (resourceClass) {
    case DxilResourceClass::eSRV:
        return "an SRV";
    case DxilResourceClass::eUAV:
        return "a UAV";
    case DxilResourceClass::eCBV:
        return "a CBV";
    case DxilResourceClass::eSampler:
        return "a sampler";
    }
    return "a resource";
}

// Moves every PSV0 resource but the root data into `info.bindings`; sets hasRootData.
std::string sortReservedSpace(const std::vector<DxilBinding> &resources, DxilInfo &info) {
    uint32 reservedCount = 0;
    for (const DxilBinding &binding : resources) {
        if (binding.space != kRootDataSpace) {
            info.bindings.push_back(binding);
            continue;
        }
        ++reservedCount;
        const bool isRootData =
            binding.resourceClass == DxilResourceClass::eCBV && binding.lowerBound == 0 && binding.count == 1;
        if (!isRootData) {
            return std::format("space 999 is reserved for EQ_ROOT_DATA (register(b0, space999)), but the shader "
                               "declares {} at register({}{}, space999)",
                               className(binding.resourceClass), registerLetter(binding.resourceClass),
                               binding.lowerBound);
        }
        info.hasRootData = true;
    }
    if (reservedCount > 1) {
        return std::format("space 999 is reserved for EQ_ROOT_DATA, but the shader declares {} resources there",
                           reservedCount);
    }
    return {};
}

// DXIL part: program header (u32 version, u32 size in dwords), then the bitcode
// header ("DXIL", u32 version, u32 bitcode offset from "DXIL", u32 bitcode size).
constexpr uint64 kDxilProgramHeaderSize = 24;
// ── ISG1, OSG1, PSG1: signatures ─────────────────────────────────────────────
// Header: u32 element count, u32 offset of the first element (from the part's
// start). Each element, 32 bytes: u32 stream, u32 name offset (from the part's
// start, NUL-terminated), u32 semantic index, u32 system value (D3D_NAME),
// u32 component type, u32 register, u8 mask, u8 read/write mask, u16 padding,
// u32 minimum precision.

constexpr uint64 kSignatureElementSize = 32;

std::string readSignature(std::span<const std::byte> part, std::string_view name,
                          std::vector<DxilSignatureElement> &elements) {
    if (part.size() < 8) {
        return std::format("the {} part is truncated", name);
    }
    const uint64 count = loadU32(part, 0);
    const uint64 first = loadU32(part, 4);
    if (first > part.size() || count > (part.size() - first) / kSignatureElementSize) {
        return std::format("the {} part's {} elements do not fit in it", name, count);
    }
    for (uint64 i = 0; i < count; ++i) {
        const uint64 at = first + i * kSignatureElementSize;
        const uint64 nameOffset = loadU32(part, at + 4);
        if (nameOffset >= part.size()) {
            return std::format("a {} element's semantic name lies outside the part", name);
        }
        DxilSignatureElement element;
        uint64 end = nameOffset;
        while (end < part.size() && part[end] != std::byte{0}) {
            element.semanticName.push_back(static_cast<char>(part[end]));
            ++end;
        }
        if (end == part.size()) {
            return std::format("a {} element's semantic name is not terminated", name);
        }
        element.semanticIndex = loadU32(part, at + 8);
        element.systemValue = loadU32(part, at + 12);
        element.registerIndex = loadU32(part, at + 20);
        element.mask = std::to_integer<uint8>(part[at + 24]);
        elements.push_back(std::move(element));
    }
    return {};
}

constexpr uint64 kBitcodeHeaderOffset = 8;

std::string findBitcode(std::span<const std::byte> part, std::span<const std::byte> &bitcode) {
    if (part.size() < kDxilProgramHeaderSize) {
        return "the DXIL part is too small for its program header";
    }
    if (loadU32(part, kBitcodeHeaderOffset) != fourCC('D', 'X', 'I', 'L')) {
        return "the DXIL part's program header lacks the 'DXIL' magic";
    }
    const uint64 start = kBitcodeHeaderOffset + loadU32(part, 16);
    const uint64 size = loadU32(part, 20);
    if (start > part.size() || size > part.size() - start) {
        return "the DXIL part's bitcode lies outside the part";
    }
    bitcode = part.subspan(start, size);
    return {};
}

std::string readInto(std::span<const std::byte> bytecode, DxilInfo &info) {
    Parts parts;
    std::string error = findParts(bytecode, parts);
    if (!error.empty()) {
        return error;
    }

    if (parts.sfi0.size() < 8) {
        return "the SFI0 part is truncated";
    }
    const uint64 features = loadU64(parts.sfi0, 0);
    info.resourceHeapIndexing = (features & kDxilFeatureResourceDescriptorHeapIndexing) != 0;
    info.samplerHeapIndexing = (features & kDxilFeatureSamplerDescriptorHeapIndexing) != 0;
    info.int64HeapAtomics = (features & kDxilFeatureAtomicInt64OnDescriptorHeapResource) != 0;
    info.int64SharedAtomics = (features & kDxilFeatureAtomicInt64OnGroupShared) != 0;

    std::vector<DxilBinding> resources;
    error = readPsv(parts.psv0, info, resources);
    if (error.empty()) {
        error = sortReservedSpace(resources, info);
    }
    if (!error.empty()) {
        return error;
    }

    const std::pair<std::span<const std::byte>, bool> signatures[] = {
        {parts.isg1, parts.hasIsg1}, {parts.osg1, parts.hasOsg1}, {parts.psg1, parts.hasPsg1}};
    std::vector<DxilSignatureElement> *targets[] = {&info.inputs, &info.outputs, &info.primitiveOutputs};
    const std::string_view names[] = {"ISG1", "OSG1", "PSG1"};
    for (size_t i = 0; i < 3; ++i) {
        if (signatures[i].second) {
            error = readSignature(signatures[i].first, names[i], *targets[i]);
            if (!error.empty()) {
                return error;
            }
        }
    }

    // The bitcode is always parsed, so a corrupt DXIL part is reported even
    // when nothing in it is needed.
    std::span<const std::byte> bitcodeBytes;
    error = findBitcode(parts.dxil, bitcodeBytes);
    if (!error.empty()) {
        return error;
    }
    Module module;
    error = parseBitcode(bitcodeBytes, module);
    if (!error.empty()) {
        return error;
    }

    std::optional<uint32> rootDataSize;
    error = findRootDataSize(module, rootDataSize);
    if (!error.empty()) {
        return error;
    }
    if (info.hasRootData != rootDataSize.has_value()) {
        return info.hasRootData
                   ? "the root data CBV (b0, space999) is missing from the DXIL part's dx.resources metadata"
                   : "the DXIL part's dx.resources metadata lists a root data CBV that PSV0 does not";
    }
    info.rootDataSize = rootDataSize.value_or(0);

    if (info.threadGroupSize[0] == 0) {
        readNumThreadsMetadata(module, info.threadGroupSize);
    }
    return {};
}

} // namespace

DxilReadResult readDxil(std::span<const std::byte> bytecode) {
    DxilReadResult result;
    result.error = readInto(bytecode, result.info);
    if (!result.error.empty()) {
        result.info = {}; // nothing partial on failure
    }
    return result;
}

std::span<const std::byte> findDxilPart(std::span<const std::byte> bytecode, std::string_view code) {
    if (code.size() != 4 || bytecode.size() < kContainerHeaderSize ||
        loadU32(bytecode, 0) != fourCC('D', 'X', 'B', 'C')) {
        return {};
    }
    const uint32 wanted = fourCC(code[0], code[1], code[2], code[3]);
    const uint64 containerSize = loadU32(bytecode, 24);
    if (containerSize < kContainerHeaderSize || containerSize > bytecode.size()) {
        return {};
    }
    const uint64 partCount = loadU32(bytecode, 28);
    if (partCount > (containerSize - kContainerHeaderSize) / 4) {
        return {};
    }
    for (uint64 i = 0; i < partCount; ++i) {
        const uint64 offset = loadU32(bytecode, kContainerHeaderSize + (4 * i));
        if (offset > containerSize - kPartHeaderSize) {
            return {};
        }
        const uint64 size = loadU32(bytecode, offset + 4);
        if (size > containerSize - offset - kPartHeaderSize) {
            return {};
        }
        if (loadU32(bytecode, offset) == wanted) {
            return bytecode.subspan(offset + kPartHeaderSize, size);
        }
    }
    return {};
}

} // namespace RI::Reflection
