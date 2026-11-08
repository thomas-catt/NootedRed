#pragma once
#include <MetalPatch/CacheResolver.hpp>

// MetalPatch::Validation：只读补丁清单类型；描述离线证据，不持有运行时地址，也不参与写入。

namespace MetalPatch::Validation
{

    // 功能组标识；系统和设备准入仍由运行时策略控制。
    enum class FeatureId : UInt32
    {
        DisplaySRD,
        DisplayLegacyBlend,
        DisplayComputeScratch,
        DisplaySubmitGuard,
        DisplayPipelineConstructor,
        DisplaySubmit,
        Count,
    };

    // 证据等级；离线语义证据不自动生成可安装补丁。
    enum class EvidenceKind : UInt8
    {
        ExactValidatedBuild,
        RelocationAwareFullFunction,
        OfflineProvenSemanticVariant,
    };

    // 系统位掩码按 Darwin 主版本编号，不使用 macOS 市场版本号。
    constexpr UInt32 OsDarwin23 = 1U << 0;
    constexpr UInt32 OsDarwin24 = 1U << 1;
    constexpr UInt32 OsDarwin25 = 1U << 2;
    constexpr UInt32 OsAll      = OsDarwin23 | OsDarwin24 | OsDarwin25;

    constexpr UInt32 NoFunction = 0xFFFFFFFFU;

    struct PatchSite
    {
        UInt32       functionIndex;
        UInt32       instructionOffset;
        UInt32       size;
        const UInt8* original;
        const UInt8* replacement;
    };

    struct ResolverView
    {
        const MetalPatch::CacheResolver::ImageReference* images;
        UInt32                                           imageCount;
        const MetalPatch::CacheResolver::Function*       functions;
        UInt32                                           functionCount;
        const MetalPatch::CacheResolver::Location*       locations;
        UInt32                                           locationCount;
        const PatchSite*                                 sites;
        UInt32                                           siteCount;
    };

    struct FeatureManifest
    {
        FeatureId    feature;
        UInt32       osMask;
        UInt32       deviceID;    // 0 表示该功能组共用同一份证据
        EvidenceKind evidence;
        ResolverView resolver;
        const char*  source;       // 生成数据头
        const char*  cacheUUID;    // 参考缓存 UUID，仅作溯源，不作兼容证明
    };

    // makeSites 在编译期把生成的 Patch[] 与 Location[] 对齐；不复制字节。
    template<typename Patch, size_t N>
    struct SiteArray
    {
        PatchSite sites[N];
    };

    template<typename Patch, size_t N>
    constexpr SiteArray<Patch, N> makeSites(const Patch                               (&patches)[N],
                                            const MetalPatch::CacheResolver::Location (&locations)[N])
    {
        SiteArray<Patch, N> result{};
        for (size_t i = 0; i < N; ++i) {
            result.sites[i] = {locations[i].function, locations[i].offset, patches[i].size, patches[i].original,
                               patches[i].patched};
        }
        return result;
    }

    // 没有完整函数证据的静态表（例如 SRD / LegacyBlend）只记录补丁位置。
    template<typename Patch, size_t N>
    constexpr SiteArray<Patch, N> makeSites(const Patch (&patches)[N])
    {
        SiteArray<Patch, N> result{};
        for (size_t i = 0; i < N; ++i) {
            result.sites[i] = {NoFunction, 0, patches[i].size, patches[i].original, patches[i].patched};
        }
        return result;
    }

    constexpr bool valid(const ResolverView& view)
    {
        if (view.imageCount > 16 || view.functionCount > 32 || view.locationCount > 32 || view.siteCount > 32) {
            return false;
        }
        if ((view.imageCount != 0 && view.images == nullptr) || (view.functionCount != 0 && view.functions == nullptr)
            || (view.siteCount != 0 && view.sites == nullptr) || (view.locationCount != 0 && view.locations == nullptr))
        {
            return false;
        }
        // Scratch/Submit 只有函数证据、没有静态补丁表；其余功能组的站点不能多于位置表。
        if (view.locations != nullptr && view.sites != nullptr && view.siteCount > view.locationCount) { return false; }
        return true;
    }

    constexpr bool valid(const FeatureManifest& manifest)
    { return valid(manifest.resolver) && manifest.source != nullptr; }

    template<size_t N>
    constexpr bool validAll(const FeatureManifest (&features)[N])
    {
        for (size_t i = 0; i < N; ++i) {
            if (!valid(features[i])) { return false; }
        }
        return true;
    }

    constexpr const FeatureManifest* find(const FeatureManifest* features, UInt32 count, FeatureId feature,
                                          UInt32 osMask, UInt32 deviceID = 0)
    {
        if (!features) { return nullptr; }
        for (UInt32 i = 0; i < count; ++i) {
            const auto& entry = features[i];
            if (entry.feature == feature && (entry.osMask & osMask) != 0
                && (entry.deviceID == 0 || entry.deviceID == deviceID))
            {
                return &entry;
            }
        }
        return nullptr;
    }

}    // namespace MetalPatch::Validation

// MetalPatch::Validation：验证后的只读计划；只引用当前地址空间中的已核验字节，不执行写入。

namespace MetalPatch::Validation
{

    constexpr UInt32 MaxPlanImages    = 16;
    constexpr UInt32 MaxPlanSites     = 32;
    constexpr UInt32 MaxPlanPatchSize = 512;

    // 与安装器的 originals/patched 判定保持一致；Mixed/Unknown 不得进入安装。
    enum class PlanState : UInt8
    {
        Unknown,
        AllOriginal,
        AllPatched,
        Mixed,
    };

    struct PlanImage
    {
        const char*                    path{nullptr};
        UInt64                         address{0};
        MetalPatch::SharedCache::Range text{};
        MetalPatch::SharedCache::Range segments[8]{};
        UInt32                         segmentCount{0};

        bool contains(UInt64 value, UInt64 size) const
        {
            for (UInt32 i = 0; i < segmentCount; ++i) {
                if (segments[i].contains(value, size)) { return true; }
            }
            return false;
        }
    };

    struct PlanSite
    {
        UInt64       address{0};    // 当前地址空间绝对地址
        UInt64       offset{0};     // 相对缓存基址
        UInt32       size{0};
        UInt32       functionIndex{NoFunction};
        UInt32       instructionOffset{0};
        const UInt8* original{nullptr};
        const UInt8* replacement{nullptr};
    };

    class VerifiedPatchPlan
    {
        const FeatureManifest*         manifest_{nullptr};
        UInt64                         base_{0};
        MetalPatch::SharedCache::Range cache_{};
        UInt32                         imageCount_{0}, siteCount_{0}, reads_{0};
        PlanImage                      images_[MaxPlanImages]{};
        PlanSite                       sites_[MaxPlanSites]{};
        PlanState                      state_{PlanState::Unknown};
        bool                           valid_{false};

    public:
        void reset(const FeatureManifest& manifest, UInt64 base, const MetalPatch::SharedCache::Range& cache)
        {
            manifest_   = &manifest;
            base_       = base;
            cache_      = cache;
            imageCount_ = siteCount_ = reads_ = 0;
            state_                            = PlanState::Unknown;
            valid_                            = false;
        }

        // 显示组可把实际解析出的补丁先装入计划，再按读回结果 seal；
        // 此时不绑定视频 FeatureManifest，站点也不得携带函数索引。
        void reset(const UInt64 base, const MetalPatch::SharedCache::Range& cache)
        {
            manifest_   = nullptr;
            base_       = base;
            cache_      = cache;
            imageCount_ = siteCount_ = reads_ = 0;
            state_                            = PlanState::Unknown;
            valid_                            = false;
        }

        bool addImage(const char* path, UInt64 address, const MetalPatch::SharedCache::Range& text,
                      const MetalPatch::SharedCache::Range* segments, UInt32 segmentCount)
        {
            if (imageCount_ == MaxPlanImages || !path || address == 0 || text.size == 0 || segmentCount > 8
                || (segmentCount != 0 && segments == nullptr) || address >= MetalPatch::SharedCache::UserLimit
                || text.address >= MetalPatch::SharedCache::UserLimit
                || text.size > MetalPatch::SharedCache::UserLimit - text.address
                || (cache_.size != 0 && (!cache_.contains(address, 1) || !cache_.contains(text.address, text.size))))
            {
                return false;
            }
            auto& image        = images_[imageCount_++];
            image.path         = path;
            image.address      = address;
            image.text         = text;
            image.segmentCount = segmentCount;
            for (UInt32 i = 0; i < segmentCount; ++i) { image.segments[i] = segments[i]; }
            return true;
        }

        bool addSite(UInt64 address, UInt64 offset, UInt32 size, UInt32 functionIndex, UInt32 instructionOffset,
                     const UInt8* original, const UInt8* replacement)
        {
            if (siteCount_ == MaxPlanSites || size == 0 || size > MaxPlanPatchSize || !original || !replacement
                || address < base_ || address - base_ != offset || address >= MetalPatch::SharedCache::UserLimit
                || size > MetalPatch::SharedCache::UserLimit - address
                || (functionIndex != NoFunction
                    && (!manifest_ || manifest_->resolver.functionCount == 0
                        || functionIndex >= manifest_->resolver.functionCount)))
            {
                return false;
            }
            // 半开区间相交即拒绝（含部分重叠）；上方已保证 address + size 不越过 UserLimit，不会溢出。
            for (UInt32 i = 0; i < siteCount_; ++i) {
                const auto& other = sites_[i];
                if (address < other.address + other.size && other.address < address + size) { return false; }
            }
            sites_[siteCount_++] = {address, offset, size, functionIndex, instructionOffset, original, replacement};
            return true;
        }

        void setReads(UInt32 reads) { reads_ = reads; }

        bool seal(PlanState state)
        {
            valid_ = base_ != 0 && cache_.size != 0 && siteCount_ != 0;
            for (UInt32 i = 0; valid_ && i < siteCount_; ++i) {
                if (!cache_.contains(sites_[i].address, sites_[i].size)) { valid_ = false; }
            }
            state_ = state;
            return valid_;
        }

        bool valid() const { return valid_; }
        bool installable() const
        { return valid_ && (state_ == PlanState::AllOriginal || state_ == PlanState::AllPatched); }
        const FeatureManifest* manifest() const { return manifest_; }
        FeatureId              feature() const { return manifest_ ? manifest_->feature : FeatureId::Count; }
        EvidenceKind evidence() const { return manifest_ ? manifest_->evidence : EvidenceKind::ExactValidatedBuild; }
        PlanState    state() const { return state_; }
        UInt64       base() const { return base_; }
        UInt32       reads() const { return reads_; }
        UInt32       imageCount() const { return imageCount_; }
        UInt32       siteCount() const { return siteCount_; }
        const PlanImage& image(UInt32 index) const { return images_[index]; }
        const PlanSite&  site(UInt32 index) const { return sites_[index]; }
    };

}    // namespace MetalPatch::Validation
