#pragma once

// The ABI tag of RI's inline namespace (P110). Everything in RI:: lives in
// RI::EQUINE_RI_ABI, so code built against the Agility SDK preview and code
// built against a retail SDK cannot link together by mistake. User code keeps
// writing RI::. EQUINE_AGILITY_SDK_PREVIEW comes with the EquineRI target.

#ifndef EQUINE_AGILITY_SDK_PREVIEW
#error "Link EquineRI: it defines EQUINE_AGILITY_SDK_PREVIEW, which picks RI's ABI tag."
#endif

#if EQUINE_AGILITY_SDK_PREVIEW
#define EQUINE_RI_ABI abi_preview
#else
#define EQUINE_RI_ABI abi_retail
#endif
