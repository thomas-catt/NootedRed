#pragma once
#include <IOKit/IOTypes.h>
#include <MetalPatch/CacheResolver.hpp>
#include <MetalPatch/SharedCache.hpp>
#include <string.h>

// MetalPatch::Validation：共享缓存解析的冷/热路径计数；只做诊断，不参与匹配或写入决策。

namespace MetalPatch::Validation
{

    enum class ResolutionEvent : UInt32
    {
        Cold,
        Hot,
        Hit,
        Miss,
        Reusable,
        HintOnly,
        Fallback,
        Collision,
        Full,
        VerifyFailure,
        ScanBytes,
        VerifyBytes,
        Candidates,
        Count
    };

    struct ResolutionCounters
    {
        UInt64 values[static_cast<UInt32>(ResolutionEvent::Count)]{};

        void record(const ResolutionEvent event, const UInt64 amount = 1)
        {
            if (event == ResolutionEvent::Count) { return; }
            __atomic_fetch_add(&values[static_cast<UInt32>(event)], amount, __ATOMIC_RELAXED);
        }

        UInt64 load(const ResolutionEvent event) const
        {
            if (event == ResolutionEvent::Count) { return 0; }
            return __atomic_load_n(&values[static_cast<UInt32>(event)], __ATOMIC_RELAXED);
        }
    };

    namespace ResolutionDiagnostics
    {

        inline ResolutionCounters counters{};
        inline bool               resolutionCacheEnabled{false};

        // 模块初始化时设置策略；初始化前不使用跨进程缓存。
        inline void setEnabled(const bool value) { resolutionCacheEnabled = value; }
        inline bool enabled() { return resolutionCacheEnabled; }

    }    // namespace ResolutionDiagnostics

}    // namespace MetalPatch::Validation

// MetalPatch::Validation：共享缓存解析的跨进程身份描述；只处理相对布局，不保存进程地址。

namespace MetalPatch::Validation
{

    constexpr UInt32 ResolutionMaxImages    = 16;
    constexpr UInt32 ResolutionMaxFunctions = 32;
    constexpr UInt32 ResolutionArchX86_64h  = 1;
    // 匹配算法或生成数据语义变化时递增，旧条目自动失效。
    constexpr UInt32 ResolutionVerifierRevision = 1;

    enum class FunctionGroup : UInt32
    {
        VideoDecode = 1,
        VideoGuard,
        DisplayCore,
        DisplayConstructor,
        DisplayScratch,
        DisplaySequoia,
        DisplaySubmit,
        Count
    };

    enum class ProofClass : UInt8
    {
        HintOnly = 0,
        // 仅当当前映射由同一可信只读对象提供且没有私有页时才可使用。
        ReusableImmutableIdentity = 1,
    };

    struct ImageSetIdentity
    {
        UInt64 low;
        UInt64 high;

        bool operator==(const ImageSetIdentity& other) const { return low == other.low && high == other.high; }
        bool operator!=(const ImageSetIdentity& other) const { return !(*this == other); }
    };

    struct ResolutionKey
    {
        UInt8            uuid[16];
        UInt32           darwinMajor;
        UInt32           arch;
        UInt32           manifestRevision;
        UInt32           profileId;
        UInt32           functionGroupId;
        ImageSetIdentity imageSet;

        bool valid() const
        {
            return darwinMajor != 0 && arch != 0 && manifestRevision != 0 && functionGroupId != 0
                   && functionGroupId < static_cast<UInt32>(FunctionGroup::Count);
        }

        bool operator==(const ResolutionKey& other) const
        {
            return memcmp(uuid, other.uuid, sizeof(uuid)) == 0 && darwinMajor == other.darwinMajor && arch == other.arch
                   && manifestRevision == other.manifestRevision && profileId == other.profileId
                   && functionGroupId == other.functionGroupId && imageSet == other.imageSet;
        }
        bool operator!=(const ResolutionKey& other) const { return !(*this == other); }
    };

    // 以缓存基址相对值描述映像；同一缓存在不同 ASLR 进程中必须得到相同值。
    struct ImageLayout
    {
        const char* path;
        UInt32      pathSize;
        UInt64      relativeStart;        // image.address - cache base
        UInt64      textRelativeStart;    // image.text.address - cache base
        UInt32      textLength;
        UInt32      mappingObjectId;    // 冷路径由内核区域查询记录
        UInt8       mappingShareMode;

        bool valid(const UInt64 cacheSize) const
        {
            return path != nullptr && pathSize != 0 && pathSize <= 256
                   && relativeStart < MetalPatch::SharedCache::UserLimit && textRelativeStart >= relativeStart
                   && textLength != 0 && textLength <= MetalPatch::SharedCache::UserLimit - textRelativeStart
                   && textRelativeStart + textLength <= relativeStart + cacheSize;
        }
    };

    inline UInt64 identityMix(UInt64 hash, UInt64 value)
    {
        // FNV-1a 64；仅用于稳定身份摘要，不单独承担内容真实性证明。
        for (UInt32 i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xFF;
            hash *= 0x00000100000001B3ULL;
        }
        return hash;
    }

    inline UInt64 identityBytes(UInt64 hash, const void* bytes, UInt32 size)
    {
        const auto* value = static_cast<const UInt8*>(bytes);
        for (UInt32 i = 0; i < size; ++i) {
            hash ^= value[i];
            hash *= 0x00000100000001B3ULL;
        }
        return hash;
    }

    inline ImageSetIdentity imageSetIdentity(const ImageLayout* images, const UInt32 count, const UInt64 cacheSize)
    {
        ImageSetIdentity identity{0x00000100000001B3ULL, 0xCBF29CE484222325ULL ^ 0x9E3779B97F4A7C15ULL};
        if (!images || count == 0 || count > ResolutionMaxImages) { return {0, 0}; }
        identity.low  = identityMix(identity.low, count);
        identity.high = identityMix(identity.high, count ^ 0xA5A5A5A5A5A5A5A5ULL);
        identity.low  = identityMix(identity.low, cacheSize);
        identity.high = identityMix(identity.high, cacheSize);
        for (UInt32 i = 0; i < count; ++i) {
            const auto& image = images[i];
            if (!image.valid(cacheSize)) { return {0, 0}; }
            identity.low           = identityBytes(identity.low, image.path, image.pathSize);
            identity.high          = identityBytes(identity.high, image.path, image.pathSize);
            const UInt64 fields[4] = {image.relativeStart, image.textRelativeStart, image.textLength,
                                      static_cast<UInt64>(image.pathSize)};
            for (const auto field : fields) {
                identity.low  = identityMix(identity.low, field);
                identity.high = identityMix(identity.high, field ^ 0x5DEECE66DULL);
            }
        }
        return identity;
    }

    // PCI 设备低 16 位与策略标志共同选择补丁规则。
    inline UInt32 resolutionProfile(const UInt32 deviceID, const UInt32 flags)
    { return (deviceID & 0xFFFFU) | (flags << 16); }

    // mach/vm_region.h 的 SM_TRUESHARED；MetalPatch::Validation 层不包含内核 VM 头，由调用方适配。
    constexpr UInt8 MappingShareTrueShared = 5;

    struct MappingInfo
    {
        UInt64 start;
        UInt64 size;
        UInt32 depth;
        UInt32 objectId;
        UInt8  shareMode;
        UInt32 shadowDepth;
        bool   isSubmap;
        bool   writable;
    };

    // 调用方提供的当前 map 区域查询；只返回属性，不读取用户内容。
    using MappingProbe = bool (*)(void* context, UInt64 address, UInt32 depth, MappingInfo& info);

    struct MappingFacts
    {
        UInt32 objectId;
        UInt8  shareMode;
        UInt32 shadowDepth;
        bool   writable;
    };

    // 要求整个范围由同一非私有只读共享对象覆盖；任何 COW/私有页或混合对象都拒绝。
    inline bool inspectMapping(const MappingProbe probe, void* context, const UInt64 address, const UInt64 size,
                               MappingFacts& facts)
    {
        if (!probe || address == 0 || size == 0 || address >= MetalPatch::SharedCache::UserLimit
            || size > MetalPatch::SharedCache::UserLimit - address)
        {
            return false;
        }
        constexpr UInt32 MaxLevels = 8;
        struct Level
        {
            UInt32 depth;
            UInt64 end;
        } levels[MaxLevels];
        UInt32 levelCount = 0, depth = 0, regions = 0;
        UInt64 cursor = address, end = address + size;
        UInt32 objectId = 0;
        bool   found    = false;
        while (cursor < end) {
            if (++regions > 4096) { return false; }
            MappingInfo info{};
            if (!probe(context, cursor, depth, info) || info.start > cursor || info.size == 0
                || info.size > MetalPatch::SharedCache::UserLimit - info.start)
            {
                return false;
            }
            const UInt64 regionEnd = info.start + info.size;
            if (regionEnd <= cursor) { return false; }
            if (info.isSubmap) {
                // 子映射只用于找到叶子区域；实际证据必须来自叶子条目。
                if (levelCount == MaxLevels || info.depth >= MaxLevels) { return false; }
                levels[levelCount++] = {depth, regionEnd};
                depth                = info.depth + 1;
                continue;
            }
            if (info.shareMode != MappingShareTrueShared || info.shadowDepth != 0 || info.writable
                || info.objectId == 0)
            {
                return false;
            }
            if (!found) {
                objectId = info.objectId;
                found    = true;
            }
            else if (info.objectId != objectId) {
                return false;
            }
            cursor = regionEnd;
            while (levelCount != 0 && cursor >= levels[levelCount - 1].end) { depth = levels[--levelCount].depth; }
        }
        if (!found) { return false; }
        facts = {objectId, MappingShareTrueShared, 0, false};
        return true;
    }

}    // namespace MetalPatch::Validation

// MetalPatch::Validation：有界、不可变的解析位置缓存；条目只保存相对布局，不引用 map 或进程内存。

namespace MetalPatch::Validation
{

    struct CachedImageLocation
    {
        UInt32 imageIndex;
        UInt32 mappingObjectId;
        UInt8  mappingShareMode;
        UInt64 relativeStart;
        UInt64 textRelativeStart;
        UInt32 textLength;
    };

    struct CachedFunctionLocation
    {
        UInt32 imageIndex;
        UInt32 functionIndex;
        // 0 为原始函数，1 为生成数据中的已补丁变体；不表示本进程已安装。
        UInt8  variant;
        UInt64 imageRelativeRVA;
    };

    struct CachedResolution
    {
        ResolutionKey          key;
        ProofClass             proof;
        UInt32                 imageCount;
        UInt32                 functionCount;
        UInt32                 flags;
        UInt32                 resolverRevision;
        UInt64                 cacheSize;
        UInt64                 proofFingerprint;
        CachedImageLocation    images[ResolutionMaxImages];
        CachedFunctionLocation functions[ResolutionMaxFunctions];
    };

    enum class PublishResult : UInt8
    {
        Invalid,
        Published,
        Duplicate,
        Conflict,
        Full,
        Disabled,
    };

    enum class ResolutionStat : UInt32
    {
        LookupHit,
        LookupMiss,
        Published,
        Duplicate,
        Conflict,
        Full,
        Invalid,
        Disabled,
        Upgraded,
        Count
    };

    struct ResolutionCacheEntry
    {
        UInt32           state;    // 0 空，1 可用，2 冲突禁用
        CachedResolution value;
    };

    // 固定容量；满表不淘汰、不覆盖。发布与快照复制都在短锁内完成，
    // 临界区只有固定结构复制，不读取用户内存、不等待其它 map。
    struct VerifiedResolutionCache
    {
        static constexpr UInt32 Capacity = 32;

        UInt32               ready;
        UInt32               guard;
        ResolutionCacheEntry entries[Capacity];
        UInt64               stats[static_cast<UInt32>(ResolutionStat::Count)];

        void init() { ready = 1; }

        void record(const ResolutionStat stat, const UInt64 amount = 1)
        { __atomic_fetch_add(&stats[static_cast<UInt32>(stat)], amount, __ATOMIC_RELAXED); }

        UInt64 stat(const ResolutionStat value) const
        { return __atomic_load_n(&stats[static_cast<UInt32>(value)], __ATOMIC_RELAXED); }

        // 复制稳定快照后立即释放锁；调用方不得在持锁期间读取用户内存。
        bool lookup(const ResolutionKey& key, CachedResolution& out)
        {
            if (!ready) {
                record(ResolutionStat::LookupMiss);
                return false;
            }
            acquire();
            bool found = false;
            for (UInt32 i = 0; i < Capacity; ++i) {
                const auto& entry = entries[i];
                if (entry.state == 1 && entry.value.key == key) {
                    out   = entry.value;
                    found = true;
                    break;
                }
            }
            release();
            record(found ? ResolutionStat::LookupHit : ResolutionStat::LookupMiss);
            return found;
        }

        PublishResult publish(const CachedResolution& value)
        {
            if (!valid(value)) {
                record(ResolutionStat::Invalid);
                return PublishResult::Invalid;
            }
            if (!ready) {
                record(ResolutionStat::Disabled);
                return PublishResult::Disabled;
            }
            acquire();
            const auto result = insert(value);
            release();
            return result;
        }

    private:
        void acquire()
        {
            while (__atomic_exchange_n(&guard, 1U, __ATOMIC_ACQUIRE) != 0) {
                while (__atomic_load_n(&guard, __ATOMIC_RELAXED) != 0) { }
            }
        }
        void release() { __atomic_store_n(&guard, 0U, __ATOMIC_RELEASE); }

        static bool valid(const CachedResolution& value)
        {
            if (!value.key.valid() || value.resolverRevision == 0
                || static_cast<UInt32>(value.proof) > static_cast<UInt32>(ProofClass::ReusableImmutableIdentity))
            {
                return false;
            }
            if (value.imageCount == 0 || value.imageCount > ResolutionMaxImages || value.functionCount == 0
                || value.functionCount > ResolutionMaxFunctions || value.cacheSize == 0)
            {
                return false;
            }
            for (UInt32 i = 0; i < value.imageCount; ++i) {
                const auto& image = value.images[i];
                if (image.imageIndex != i || image.textLength == 0
                    || image.relativeStart >= MetalPatch::SharedCache::UserLimit
                    || image.textRelativeStart < image.relativeStart
                    || image.textLength > MetalPatch::SharedCache::UserLimit - image.textRelativeStart
                    || image.textRelativeStart + image.textLength > image.relativeStart + value.cacheSize)
                {
                    return false;
                }
                if (value.proof == ProofClass::ReusableImmutableIdentity
                    && (image.mappingObjectId == 0 || image.mappingShareMode == 0))
                {
                    return false;
                }
            }
            for (UInt32 i = 0; i < value.functionCount; ++i) {
                const auto& function = value.functions[i];
                if (function.functionIndex >= value.functionCount || function.imageIndex >= value.imageCount
                    || function.variant > 1 || function.imageRelativeRVA >= MetalPatch::SharedCache::UserLimit)
                {
                    return false;
                }
                for (UInt32 j = 0; j < i; ++j) {
                    if (value.functions[j].functionIndex == function.functionIndex) { return false; }
                }
            }
            return true;
        }

        // 布局与函数位置一致时才允许同一键继续使用；映射身份作为可复用证明的附加证据。
        static bool sameLayout(const CachedResolution& left, const CachedResolution& right)
        {
            if (left.resolverRevision != right.resolverRevision || left.flags != right.flags
                || left.cacheSize != right.cacheSize || left.imageCount != right.imageCount
                || left.functionCount != right.functionCount)
            {
                return false;
            }
            for (UInt32 i = 0; i < left.imageCount; ++i) {
                const auto& a = left.images[i];
                const auto& b = right.images[i];
                if (a.imageIndex != b.imageIndex || a.relativeStart != b.relativeStart
                    || a.textRelativeStart != b.textRelativeStart || a.textLength != b.textLength)
                {
                    return false;
                }
            }
            for (UInt32 i = 0; i < left.functionCount; ++i) {
                const auto& a = left.functions[i];
                const auto& b = right.functions[i];
                if (a.imageIndex != b.imageIndex || a.functionIndex != b.functionIndex || a.variant != b.variant
                    || a.imageRelativeRVA != b.imageRelativeRVA)
                {
                    return false;
                }
            }
            return true;
        }

        static bool sameEvidence(const CachedResolution& left, const CachedResolution& right)
        {
            for (UInt32 i = 0; i < left.imageCount; ++i) {
                if (left.images[i].mappingObjectId != right.images[i].mappingObjectId
                    || left.images[i].mappingShareMode != right.images[i].mappingShareMode)
                {
                    return false;
                }
            }
            return true;
        }

        PublishResult insert(const CachedResolution& value)
        {
            for (UInt32 i = 0; i < Capacity; ++i) {
                auto& entry = entries[i];
                if (entry.state == 0 || entry.value.key != value.key) { continue; }
                if (entry.state == 1 && sameLayout(entry.value, value)) {
                    if (value.proof == ProofClass::ReusableImmutableIdentity
                        && entry.value.proof == ProofClass::HintOnly)
                    {
                        // HintOnly 不参与热路径；允许同布局的较强证明替换条目。
                        entry.value = value;
                        record(ResolutionStat::Upgraded);
                        record(ResolutionStat::Published);
                        return PublishResult::Published;
                    }
                    if (entry.value.proof == ProofClass::ReusableImmutableIdentity
                        && value.proof == ProofClass::ReusableImmutableIdentity && !sameEvidence(entry.value, value))
                    {
                        entry.state = 2;
                        record(ResolutionStat::Conflict);
                        return PublishResult::Conflict;
                    }
                    record(ResolutionStat::Duplicate);
                    return PublishResult::Duplicate;
                }
                // 相同键出现不同位置证据时禁用条目，后续继续走完整扫描。
                entry.state = 2;
                record(ResolutionStat::Conflict);
                return PublishResult::Conflict;
            }
            for (UInt32 i = 0; i < Capacity; ++i) {
                auto& entry = entries[i];
                if (entry.state != 0) { continue; }
                entry.value = value;
                entry.state = 1;
                record(ResolutionStat::Published);
                return PublishResult::Published;
            }
            record(ResolutionStat::Full);
            return PublishResult::Full;
        }
    };

    inline VerifiedResolutionCache resolutionCache;

}    // namespace MetalPatch::Validation

// MetalPatch::Validation：冷/热解析共用的身份观察与缓存查发；不修改用户内存，不宣布功能已安装。

namespace MetalPatch::Validation
{

    // 由调用方在每个功能组开始时重置；所有证据只对当前 map 的本次调用有效。
    struct FastResolution
    {
        const UInt8* uuid;
        UInt32       darwinMajor;
        UInt32       profileId;
        UInt32       flags;
        MappingProbe probe;
        void*        probeContext;
        UInt64       cacheBase;
        UInt64       cacheSize;
        UInt32       imageCount;
        UInt64       imageAddresses[ResolutionMaxImages];
        // 布局与查发快照保存在调用方工作区中，避免大数组占用内核栈。
        ImageLayout      layouts[ResolutionMaxImages];
        UInt32           objectIds[ResolutionMaxImages];
        CachedResolution snapshot;
        CachedResolution staging;
        ImageSetIdentity identity;
        bool             mappingClean;
        bool             observed;
        bool             hot;
        bool             enabled;

        void begin(const UInt8* cacheUUID, const UInt32 major, const UInt32 profile, const UInt32 policyFlags,
                   const MappingProbe mappingProbe, void* mappingContext)
        {
            uuid         = cacheUUID;
            darwinMajor  = major;
            profileId    = profile;
            flags        = policyFlags;
            probe        = mappingProbe;
            probeContext = mappingContext;
            enabled =
                ResolutionDiagnostics::enabled() && resolutionCache.ready != 0 && uuid != nullptr && probe != nullptr;
            restart();
        }

        // 同一调用内切换功能组时保留 uuid/profile/探针，只清除当前映像证据。
        void restart()
        {
            cacheBase = cacheSize = 0;
            imageCount            = 0;
            identity              = {0, 0};
            mappingClean = observed = hot = false;
            memset(imageAddresses, 0, sizeof(imageAddresses));
            memset(layouts, 0, sizeof(layouts));
            memset(objectIds, 0, sizeof(objectIds));
        }

        // 只读取映像布局与当前 map 的区域属性；不扫描文本、不写页面。
        bool observeImages(const UInt64 base, const UInt64 size, const MetalPatch::CacheResolver::ImageBounds* bounds,
                           const MetalPatch::CacheResolver::ImageReference* refs, const UInt32 count)
        {
            observed = false;
            if (!enabled || !bounds || !refs || count == 0 || count > ResolutionMaxImages || base == 0 || size == 0
                || base >= MetalPatch::SharedCache::UserLimit || size > MetalPatch::SharedCache::UserLimit - base)
            {
                return false;
            }
            for (UInt32 i = 0; i < count; ++i) {
                if (!refs[i].path || refs[i].size == 0 || refs[i].size > 256 || bounds[i].address < base
                    || bounds[i].text.address < base || bounds[i].text.size == 0 || bounds[i].text.size > 0xFFFFFFFFULL)
                {
                    return false;
                }
                auto& layout             = layouts[i];
                layout.path              = refs[i].path;
                layout.pathSize          = refs[i].size;
                layout.relativeStart     = bounds[i].address - base;
                layout.textRelativeStart = bounds[i].text.address - base;
                layout.textLength        = static_cast<UInt32>(bounds[i].text.size);
                layout.mappingObjectId   = 0;
                layout.mappingShareMode  = 0;
                if (!layout.valid(size)) { return false; }
                imageAddresses[i] = bounds[i].address;
            }
            identity = imageSetIdentity(layouts, count, size);
            if (identity.low == 0 && identity.high == 0) { return false; }
            cacheBase    = base;
            cacheSize    = size;
            imageCount   = count;
            mappingClean = true;
            for (UInt32 i = 0; i < count; ++i) {
                MappingFacts facts{};
                if (!inspectMapping(probe, probeContext, bounds[i].text.address, bounds[i].text.size, facts)) {
                    mappingClean = false;
                    objectIds[i] = 0;
                    continue;
                }
                objectIds[i] = facts.objectId;
            }
            observed = true;
            return true;
        }

        ResolutionKey key(const FunctionGroup group) const
        {
            ResolutionKey value{};
            memcpy(value.uuid, uuid, sizeof(value.uuid));
            value.darwinMajor      = darwinMajor;
            value.arch             = ResolutionArchX86_64h;
            value.manifestRevision = ResolutionVerifierRevision;
            value.profileId        = profileId;
            value.functionGroupId  = static_cast<UInt32>(group);
            value.imageSet         = identity;
            return value;
        }

        // 仅当快照带可复用证明、布局与内核映射身份都匹配时才填充已知位置。
        bool lookup(const FunctionGroup group, const UInt32* functionImages, const UInt32 functionCount,
                    MetalPatch::CacheResolver::KnownLocation* known)
        {
            hot = false;
            if (!enabled || !observed || !mappingClean || !functionImages || !known || functionCount == 0
                || functionCount > ResolutionMaxFunctions)
            {
                return false;
            }
            memset(known, 0, sizeof(MetalPatch::CacheResolver::KnownLocation) * functionCount);
            auto& cached = snapshot;
            if (!resolutionCache.lookup(key(group), cached)) { return false; }
            if (cached.proof != ProofClass::ReusableImmutableIdentity || cached.imageCount != imageCount
                || cached.functionCount == 0 || cached.functionCount > functionCount || cached.cacheSize != cacheSize)
            {
                ResolutionDiagnostics::counters.record(ResolutionEvent::Fallback);
                return false;
            }
            for (UInt32 i = 0; i < imageCount; ++i) {
                const auto& image = cached.images[i];
                if (image.relativeStart != layouts[i].relativeStart
                    || image.textRelativeStart != layouts[i].textRelativeStart
                    || image.textLength != layouts[i].textLength || image.mappingObjectId != objectIds[i]
                    || image.mappingShareMode != MappingShareTrueShared)
                {
                    ResolutionDiagnostics::counters.record(ResolutionEvent::Fallback);
                    return false;
                }
            }
            for (UInt32 i = 0; i < cached.functionCount; ++i) {
                const auto& function = cached.functions[i];
                if (function.functionIndex >= functionCount || function.imageIndex >= imageCount
                    || functionImages[function.functionIndex] != function.imageIndex || function.variant > 1)
                {
                    ResolutionDiagnostics::counters.record(ResolutionEvent::Fallback);
                    return false;
                }
                const auto& image   = layouts[function.imageIndex];
                const auto  address = imageAddresses[function.imageIndex] + function.imageRelativeRVA;
                const auto  text    = cacheBase + image.textRelativeStart;
                if (address < text || address - text >= image.textLength) {
                    ResolutionDiagnostics::counters.record(ResolutionEvent::Fallback);
                    return false;
                }
                known[function.functionIndex] = {address, function.variant != 0};
            }
            hot = true;
            ResolutionDiagnostics::counters.record(ResolutionEvent::Hot);
            ResolutionDiagnostics::counters.record(ResolutionEvent::Hit);
            return true;
        }

        // 完整冷扫描成功后发布；只有整段文本都由同一可信共享对象提供时才可升级为 Reusable。
        void publish(const FunctionGroup group, const MetalPatch::CacheResolver::KnownLocation* locations,
                     const UInt32* functionImages, const UInt32 functionCount)
        {
            if (!enabled || !observed || !locations || !functionImages || functionCount == 0
                || functionCount > ResolutionMaxFunctions || imageCount == 0)
            {
                return;
            }
            auto& value = staging;
            memset(&value, 0, sizeof(value));
            value.key              = key(group);
            value.proof            = mappingClean ? ProofClass::ReusableImmutableIdentity : ProofClass::HintOnly;
            value.imageCount       = imageCount;
            value.functionCount    = functionCount;
            value.flags            = flags;
            value.resolverRevision = ResolutionVerifierRevision;
            value.cacheSize        = cacheSize;
            UInt64 fingerprint     = identity.low ^ identity.high;
            for (UInt32 i = 0; i < imageCount; ++i) {
                value.images[i].imageIndex        = static_cast<UInt32>(i);
                value.images[i].mappingObjectId   = mappingClean ? objectIds[i] : 0;
                value.images[i].mappingShareMode  = static_cast<UInt8>(mappingClean ? MappingShareTrueShared : 0);
                value.images[i].relativeStart     = layouts[i].relativeStart;
                value.images[i].textRelativeStart = layouts[i].textRelativeStart;
                value.images[i].textLength        = layouts[i].textLength;
                fingerprint                       = identityMix(fingerprint, objectIds[i]);
            }
            for (UInt32 i = 0; i < functionCount; ++i) {
                const auto image = functionImages[i];
                if (image >= imageCount || locations[i].address == 0) { return; }
                const auto start =
                    imageAddresses[image] + (layouts[image].textRelativeStart - layouts[image].relativeStart);
                if (locations[i].address < start || locations[i].address - start >= layouts[image].textLength) {
                    return;
                }
                const auto rva     = locations[i].address - imageAddresses[image];
                value.functions[i] = {static_cast<UInt32>(image), static_cast<UInt32>(i),
                                      static_cast<UInt8>(locations[i].patched ? 1 : 0), rva};
                fingerprint        = identityMix(fingerprint, rva);
            }
            value.proofFingerprint = fingerprint;
            const auto result      = resolutionCache.publish(value);
            if (result == PublishResult::Published || result == PublishResult::Duplicate) {
                ResolutionDiagnostics::counters.record(mappingClean ? ResolutionEvent::Reusable :
                                                                      ResolutionEvent::HintOnly);
            }
        }

        void rejectHot()
        {
            hot = false;
            ResolutionDiagnostics::counters.record(ResolutionEvent::Fallback);
        }
    };

}    // namespace MetalPatch::Validation
