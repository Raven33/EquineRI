#include <RI/Helpers/Transfers.hpp>

#include <cstring>

namespace RI::Helpers {

namespace {

// Staging memory and one command list for a single transfer.
struct Staging {
    Device *device = nullptr;
    Memory memory;
    Buffer buffer;
    CommandList cmd;

    Staging(Device &d, MemoryKind kind, uint64 size, BufferUsage usage) : device(&d) {
        const BufferDesc desc{.size = size, .usage = usage};
        const MemoryRequirements req = bufferRequirements(d, desc);
        memory = allocateMemory(d, kind, req.size);
        buffer = createBuffer(d, memory, 0, desc);
        cmd = createCommandList(d);
        cmdBegin(cmd);
        cmdBarrier(cmd, Stage::eAll, Stage::eCopy); // after everything submitted before
    }
    void run() {
        cmdBarrier(cmd, Stage::eCopy, Stage::eAll); // before everything submitted after
        cmdEnd(cmd);
        wait(device->graphicsQueue, submit(device->graphicsQueue, cmd));
    }
    ~Staging() {
        destroyCommandList(*device, cmd);
        destroyBuffer(*device, buffer);
        freeMemory(*device, memory);
    }
};

} // namespace

void uploadBuffer(Device &device, const Buffer &dst, uint64 offset, const void *data, uint64 size) {
    Staging staging(device, MemoryKind::eUpload, size, BufferUsage::eCopySrc);
    std::memcpy(staging.buffer.mapped, data, size);
    cmdCopyBuffer(staging.cmd, staging.buffer, 0, dst, offset, size);
    staging.run();
}

std::vector<std::byte> readbackBuffer(Device &device, const Buffer &src, uint64 offset, uint64 size) {
    Staging staging(device, MemoryKind::eReadback, size, BufferUsage::eCopyDst);
    cmdCopyBuffer(staging.cmd, src, offset, staging.buffer, 0, size);
    staging.run();
    const auto *bytes = static_cast<const std::byte *>(staging.buffer.mapped);
    return {bytes, bytes + size};
}

void uploadTexture(Device &device, const TextureView &dst, const void *texels) {
    const CopyLayout layout = copyLayout(device, dst);
    Staging staging(device, MemoryKind::eUpload, layout.size, BufferUsage::eCopySrc);
    const uint64 row = uint64{layout.width} * layout.bytesPerTexel;
    auto *out = static_cast<std::byte *>(staging.buffer.mapped);
    const auto *in = static_cast<const std::byte *>(texels);
    for (uint32 layer = 0; layer < layout.layers; ++layer) {
        for (uint32 y = 0; y < layout.height; ++y) {
            std::memcpy(out + layer * layout.layerPitch + y * layout.rowPitch,
                        in + (uint64{layer} * layout.height + y) * row, row);
        }
    }
    cmdCopyBufferToTexture(staging.cmd, staging.buffer, 0, dst);
    staging.run();
}

std::vector<std::byte> readbackTexture(Device &device, const TextureView &src) {
    const CopyLayout layout = copyLayout(device, src);
    Staging staging(device, MemoryKind::eReadback, layout.size, BufferUsage::eCopyDst);
    cmdCopyTextureToBuffer(staging.cmd, src, staging.buffer, 0);
    staging.run();
    const uint64 row = uint64{layout.width} * layout.bytesPerTexel;
    std::vector<std::byte> texels(row * layout.height * layout.layers);
    const auto *in = static_cast<const std::byte *>(staging.buffer.mapped);
    for (uint32 layer = 0; layer < layout.layers; ++layer) {
        for (uint32 y = 0; y < layout.height; ++y) {
            std::memcpy(texels.data() + (uint64{layer} * layout.height + y) * row,
                        in + layer * layout.layerPitch + y * layout.rowPitch, row);
        }
    }
    return texels;
}

} // namespace RI::Helpers
