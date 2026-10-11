// Canonical D3D12: object names (SetName, UTF-16) and command markers as PIX
// events without the PIX runtime (BeginEvent with a UTF-16 string, metadata 0).
#include <d3d12.h>

#include <cwchar>

// --8<-- [start:names]
void nameResource(ID3D12Resource *resource) { resource->SetName(L"particles"); }
// --8<-- [end:names]

// --8<-- [start:markers]
void labelled(ID3D12GraphicsCommandList *list) {
    const wchar_t *name = L"particles";
    list->BeginEvent(0, name, static_cast<UINT>((std::wcslen(name) + 1) * sizeof(wchar_t)));
    // ... commands ...
    list->EndEvent();
}
// --8<-- [end:markers]
