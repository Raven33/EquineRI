// Canonical D3D12: a buffer-to-texture copy of one subresource, with the
// footprint (256-byte row pitch, 512-byte placement) from GetCopyableFootprints.
#include <d3d12.h>

// --8<-- [start:upload]
void upload(ID3D12Device *device, ID3D12GraphicsCommandList *list, ID3D12Resource *staging, ID3D12Resource *texture) {
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    // Write each of `rows` rows (rowBytes long) at footprint.Footprint.RowPitch
    // into `staging` first. The texture must be in a layout that allows copies.
    D3D12_TEXTURE_COPY_LOCATION to{.pResource = texture, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    to.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION from{.pResource = staging, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    from.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}
// --8<-- [end:upload]

// --8<-- [start:small-write]
void resetCounter(ID3D12GraphicsCommandList2 *list, ID3D12Resource *counters, UINT64 start) {
    // One parameter per 32-bit value, each with its own GPU address.
    const D3D12_GPU_VIRTUAL_ADDRESS address = counters->GetGPUVirtualAddress();
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER words[2] = {
        {address, static_cast<UINT>(start)},
        {address + 4, static_cast<UINT>(start >> 32)},
    };
    list->WriteBufferImmediate(2, words, nullptr); // nullptr: the default mode
    // Then a barrier from the copy to the shaders that read the counter.
}
// --8<-- [end:small-write]
