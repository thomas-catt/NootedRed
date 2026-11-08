// Local compatibility profile for the inspected 24H23 x86_64h shared cache.
// Upstream payload bytes are unchanged; only the Metal _MTLIOAccelResource._res
// ivar target moved. The previous RVA names an unrelated MTLBinaryArchive ivar.
#pragma once
#include <MetalPatch/Data/Sequoia.hpp>

namespace MetalPatch::Sequoia24H23
{

    inline constexpr UInt8 CacheUUID[] = {0xfb, 0x6a, 0x36, 0xbe, 0x0c, 0xcd, 0x38, 0x35,
                                          0xae, 0x8c, 0xe6, 0x00, 0xa9, 0x80, 0xe3, 0xa3};
    template<size_t N>
    struct References
    {
        MetalPatch::CacheResolver::Reference values[N];
    };
    template<size_t N>
    constexpr References<N> relocate(const MetalPatch::CacheResolver::Reference (&source)[N])
    {
        References<N> result{};
        for (size_t i = 0; i < N; ++i) {
            result.values[i] = source[i];
            auto& r          = result.values[i];
            if (r.image == 3 && r.indirect == 1 && r.rva == 0x36ACA080ULL) { r.rva = 0x36ACB080ULL; }
        }
        return result;
    }
    inline constexpr auto Original2 = relocate(MetalPatch::SequoiaData::ResolverOriginalRefs2);
    inline constexpr auto Patched2  = relocate(MetalPatch::SequoiaData::ResolverPatchedRefs2);
    inline constexpr auto Original3 = relocate(MetalPatch::SequoiaData::ResolverOriginalRefs3);
    inline constexpr auto Patched3  = relocate(MetalPatch::SequoiaData::ResolverPatchedRefs3);
    inline constexpr MetalPatch::CacheResolver::Function Functions[] = {
        MetalPatch::SequoiaData::ResolverFunctions[0],
        MetalPatch::SequoiaData::ResolverFunctions[1],
        []
        {
            auto f         = MetalPatch::SequoiaData::ResolverFunctions[2];
            f.originalRefs = Original2.values;
            f.patchedRefs  = Patched2.values;
            return f;
        }(),
        []
        {
            auto f         = MetalPatch::SequoiaData::ResolverFunctions[3];
            f.originalRefs = Original3.values;
            f.patchedRefs  = Patched3.values;
            return f;
        }(),
        MetalPatch::SequoiaData::ResolverFunctions[4],
    };

}    // namespace MetalPatch::Sequoia24H23
