// GPU-free tests of RI's ABI tag (P110): RI:: names resolve into the inline
// namespace the tree's Agility SDK picks, and the tag reaches mangled names.

#include <TestHarness.hpp>

#include <RI/Resources.hpp>

#include <string_view>
#include <type_traits>
#include <typeinfo>

EQ_TEST(abi, tag_follows_sdk) {
#if EQUINE_AGILITY_SDK_PREVIEW
    static_assert(std::is_same_v<RI::BufferDesc, RI::abi_preview::BufferDesc>);
    constexpr std::string_view kTag = "abi_preview";
#else
    static_assert(std::is_same_v<RI::BufferDesc, RI::abi_retail::BufferDesc>);
    constexpr std::string_view kTag = "abi_retail";
#endif
    EQ_CHECK(std::string_view(typeid(RI::BufferDesc).name()).find(kTag) != std::string_view::npos);
}
