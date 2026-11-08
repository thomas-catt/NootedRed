#pragma once
#include <MetalPatch/PatchPlan.hpp>

// Metal feature dependencies and per-transaction installation state.
// 本文件只描述离线证据和运行时准入边界，不保存进程地址，也不执行写入。

namespace MetalPatch::Validation
{

    // Preserve the published Metal group bit positions used by existing diagnostics.
    enum class DisplayGroup : UInt8
    {
        MetalRenderCore   = 2,
        MetalScratch      = 3,
        MetalSubmitSafety = 4
    };
    constexpr UInt32 DisplayGroupCount = 5;
    constexpr UInt32 displayGroupBit(const DisplayGroup group) { return 1U << static_cast<UInt32>(group); }
    constexpr UInt32 allDisplayGroupBits = (1U << 2) | (1U << 3) | (1U << 4);
    constexpr UInt32 displayOsMask(const UInt32 major)
    {
        switch (major) {
            case 23: return OsDarwin23;
            case 24: return OsDarwin24;
            case 25: return OsDarwin25;
            default: return 0;
        }
    }

}    // namespace MetalPatch::Validation

// MetalPatch::Validation：能力状态机与按地址空间账本；地址空间销毁前必须清理对应记录。

namespace MetalPatch::Validation
{

    // 每个 feature、每个地址空间独立推进；HardwareValidated 不属于运行时状态。
    enum class CapabilityState : UInt8
    {
        Disabled,
        Eligible,
        ImageResolved,
        Verified,
        Prepared,
        Installing,
        Installed,
        RuntimeObserved,
        CommitFailed,
        RollbackVerified,
        RollbackFailed,
        Rejected,
        Unsupported,
        Quarantined,
    };

    constexpr const char* stateName(const CapabilityState state)
    {
        switch (state) {
            case CapabilityState::Disabled        : return "Disabled";
            case CapabilityState::Eligible        : return "Eligible";
            case CapabilityState::ImageResolved   : return "ImageResolved";
            case CapabilityState::Verified        : return "Verified";
            case CapabilityState::Prepared        : return "Prepared";
            case CapabilityState::Installing      : return "Installing";
            case CapabilityState::Installed       : return "Installed";
            case CapabilityState::RuntimeObserved : return "RuntimeObserved";
            case CapabilityState::CommitFailed    : return "CommitFailed";
            case CapabilityState::RollbackVerified: return "RollbackVerified";
            case CapabilityState::RollbackFailed  : return "RollbackFailed";
            case CapabilityState::Rejected        : return "Rejected";
            case CapabilityState::Unsupported     : return "Unsupported";
            case CapabilityState::Quarantined     : return "Quarantined";
        }
        return "Unknown";
    }

    constexpr bool canTransition(const CapabilityState from, const CapabilityState to)
    {
        if (from == to) { return true; }
        switch (from) {
            case CapabilityState::Disabled:
                return to == CapabilityState::Eligible || to == CapabilityState::Rejected
                       || to == CapabilityState::Unsupported;
            case CapabilityState::Eligible:
                return to == CapabilityState::ImageResolved || to == CapabilityState::Disabled
                       || to == CapabilityState::Rejected || to == CapabilityState::Unsupported;
            case CapabilityState::ImageResolved:
                return to == CapabilityState::Verified || to == CapabilityState::Disabled
                       || to == CapabilityState::Rejected;
            case CapabilityState::Verified:
                return to == CapabilityState::Prepared || to == CapabilityState::Disabled
                       || to == CapabilityState::Rejected;
            case CapabilityState::Prepared:
                return to == CapabilityState::Installing || to == CapabilityState::Disabled
                       || to == CapabilityState::Rejected;
            case CapabilityState::Installing:
                return to == CapabilityState::Installed || to == CapabilityState::CommitFailed
                       || to == CapabilityState::RollbackFailed;
            case CapabilityState::Installed:
                return to == CapabilityState::RuntimeObserved || to == CapabilityState::Disabled;
            case CapabilityState::RuntimeObserved: return to == CapabilityState::Disabled;
            case CapabilityState::CommitFailed:
                return to == CapabilityState::RollbackVerified || to == CapabilityState::RollbackFailed;
            case CapabilityState::RollbackVerified: return to == CapabilityState::Disabled;
            case CapabilityState::RollbackFailed  : return to == CapabilityState::Quarantined;
            case CapabilityState::Rejected        : return to == CapabilityState::Disabled;
            case CapabilityState::Unsupported     : return false;    // 本次启动不再重试
            case CapabilityState::Quarantined     : return false;    // 不自动重试
        }
        return false;
    }

    // 发布证据与运行时状态分离：运行时不能把自己标记为已在硬件验收。
    enum class ReleaseEvidence : UInt8
    {
        Unknown,
        ValidatedOnHardware,
    };

    // 有界账本，由调用方串行化；活跃 map 的隔离状态不得因进程身份变化而重置。
    template<UInt32 MaxEntries>
    class CapabilityLedgerStorage
    {
    public:
        static constexpr UInt32 Capacity = MaxEntries;

    private:
        struct Entry
        {
            UInt64          map{0};
            FeatureId       feature{FeatureId::DisplaySRD};
            CapabilityState state{CapabilityState::Disabled};
        };

        Entry  entries_[Capacity]{};
        UInt32 count_{0};

        Entry* find(const UInt64 map, const FeatureId feature)
        {
            for (UInt32 i = 0; i < count_; ++i) {
                if (entries_[i].map == map && entries_[i].feature == feature) { return &entries_[i]; }
            }
            return nullptr;
        }

        const Entry* find(const UInt64 map, const FeatureId feature) const
        {
            for (UInt32 i = 0; i < count_; ++i) {
                if (entries_[i].map == map && entries_[i].feature == feature) { return &entries_[i]; }
            }
            return nullptr;
        }

    public:
        void clear() { count_ = 0; }

        // 新条目从 Disabled 开始；非法迁移和 Quarantined 自动重试均拒绝。
        bool set(const UInt64 map, const FeatureId feature, const CapabilityState state)
        {
            if (!map) { return false; }
            auto* entry = find(map, feature);
            if (!entry) {
                if (count_ == Capacity || !canTransition(CapabilityState::Disabled, state)) { return false; }
                entry          = &entries_[count_++];
                entry->map     = map;
                entry->feature = feature;
                entry->state   = CapabilityState::Disabled;
            }
            if (!canTransition(entry->state, state)) { return false; }
            entry->state = state;
            return true;
        }

        CapabilityState state(const UInt64 map, const FeatureId feature) const
        {
            const auto* entry = find(map, feature);
            return entry ? entry->state : CapabilityState::Disabled;
        }

        void removeMap(const UInt64 map)
        {
            UInt32 write = 0;
            for (UInt32 read = 0; read < count_; ++read) {
                if (entries_[read].map != map) { entries_[write++] = entries_[read]; }
            }
            count_ = write;
        }

        UInt32 mapCount(const FeatureId feature, const CapabilityState state) const
        {
            UInt32 result = 0;
            for (UInt32 i = 0; i < count_; ++i) {
                if (entries_[i].feature == feature && entries_[i].state == state) { ++result; }
            }
            return result;
        }

        UInt32 installedMaps(const FeatureId feature) const
        { return mapCount(feature, CapabilityState::Installed) + mapCount(feature, CapabilityState::RuntimeObserved); }

        // 全局发布只能表示“至少一个已记录实例成功”，不能代表所有进程。
        bool anyInstalled(const FeatureId feature) const { return installedMaps(feature) != 0; }

        bool quarantined(const FeatureId feature) const { return mapCount(feature, CapabilityState::Quarantined) != 0; }
    };

    using CapabilityLedger = CapabilityLedgerStorage<64>;

}    // namespace MetalPatch::Validation

// MetalPatch::Validation：依赖规划器；纯函数，不读取或写入进程地址空间。

namespace MetalPatch::Validation
{

    constexpr UInt32 FeatureCount = static_cast<UInt32>(FeatureId::Count);
    static_assert(FeatureCount <= 32);

    constexpr UInt32 featureBit(const FeatureId feature) { return 1U << static_cast<UInt32>(feature); }

    constexpr UInt32 allFeatureBits = (1U << FeatureCount) - 1;

    // 外部前提由内核侧桥接/权限决定；规划器只判断是否具备，不自行初始化。
    constexpr UInt32 ExternalMetalBridge    = 1U << 1;
    constexpr UInt32 ExternalSubmitResolver = 1U << 3;

    constexpr UInt32 deviceBit(const UInt32 deviceID)
    {
        switch (deviceID) {
            case 0x15D8: return 1U << 0;
            case 0x15DD: return 1U << 1;
            case 0x15E7: return 1U << 2;
            case 0x1636: return 1U << 3;
            case 0x1638: return 1U << 4;
            case 0x164C: return 1U << 5;
            default    : return 0;
        }
    }

    constexpr UInt32 DisplayDeviceMask = (1U << 6) - 1;

    struct PlannerFeature
    {
        FeatureId        feature;
        UInt32           osMask;
        UInt32           deviceMask;
        UInt32           external;
        const FeatureId* required;
        UInt32           requireCount;
        const FeatureId* before;
        UInt32           beforeCount;
    };

    struct ConflictRule
    {
        FeatureId loser;
        FeatureId winner;
    };

    struct PlannerInput
    {
        UInt32 osMask{0};    // 单个 Darwin 位
        UInt32 deviceID{0};
        UInt32 requested{0};    // 由 boot-args/策略得出的候选功能
        UInt32 verified{0};     // 已具备映像/完整函数证据的功能
        UInt32 external{0};
    };

    struct PlannerResult
    {
        UInt32    enabled{0};
        UInt32    rejected{0};
        UInt32    missing{0};
        UInt32    conflicts{0};
        UInt32    missingExternal{0};
        FeatureId order[FeatureCount]{};
        UInt32    orderCount{0};
        bool      cycle{false};
    };

    inline constexpr FeatureId RequiresSRD[]       = {FeatureId::DisplaySRD};
    inline constexpr FeatureId BeforeSubmitGuard[] = {FeatureId::DisplaySubmitGuard};

    // 运行时实际安装顺序由 required 与 before 共同表达；before 只排序，不构成硬依赖。
    inline constexpr PlannerFeature DefaultFeatures[] = {
        {FeatureId::DisplaySubmitGuard, OsAll, DisplayDeviceMask, ExternalMetalBridge | ExternalSubmitResolver, nullptr,
         0, nullptr, 0},
        {FeatureId::DisplaySubmit, OsAll, DisplayDeviceMask, ExternalMetalBridge | ExternalSubmitResolver, nullptr, 0,
         nullptr, 0},
        {FeatureId::DisplayPipelineConstructor, OsAll, DisplayDeviceMask, ExternalMetalBridge, nullptr, 0, nullptr, 0},
        {FeatureId::DisplaySRD, OsAll, DisplayDeviceMask, ExternalMetalBridge, nullptr, 0, BeforeSubmitGuard, 1},
        {FeatureId::DisplayLegacyBlend, OsAll, DisplayDeviceMask, ExternalMetalBridge, RequiresSRD, 1, nullptr, 0},
        {FeatureId::DisplayComputeScratch, OsAll, DisplayDeviceMask, ExternalMetalBridge, RequiresSRD, 1, nullptr, 0},
    };

    constexpr const PlannerFeature* findFeature(const PlannerFeature* features, const UInt32 featureCount,
                                                const FeatureId feature)
    {
        if (!features) { return nullptr; }
        for (UInt32 i = 0; i < featureCount; ++i) {
            if (features[i].feature == feature) { return &features[i]; }
        }
        return nullptr;
    }

    namespace Detail
    {

        constexpr UInt32 indexOf(const FeatureId feature) { return static_cast<UInt32>(feature); }

        constexpr bool isEnabled(const UInt32 bits, const FeatureId feature)
        { return (bits & featureBit(feature)) != 0; }

        constexpr void set(UInt32& bits, const FeatureId feature) { bits |= featureBit(feature); }

        constexpr bool gated(const PlannerFeature& entry, const PlannerInput& input)
        {
            if ((entry.osMask & input.osMask) == 0) { return false; }
            if (entry.deviceMask != 0 && (deviceBit(input.deviceID) & entry.deviceMask) == 0) { return false; }
            return true;
        }

        constexpr bool conflictEnabled(const ConflictRule* rules, const UInt32 ruleCount, const FeatureId feature,
                                       const UInt32 enabled)
        {
            for (UInt32 i = 0; i < ruleCount; ++i) {
                if (rules[i].loser == feature && isEnabled(enabled, rules[i].winner)) { return true; }
                if (rules[i].winner == feature && isEnabled(enabled, rules[i].loser)) { return true; }
            }
            return false;
        }

    }    // namespace Detail

    // 依赖闭包 + 冲突消解 + 拓扑排序。任何前提缺失都 fail closed。
    inline PlannerResult evaluate(const PlannerFeature* features, const UInt32 featureCount,
                                  const ConflictRule* conflicts, const UInt32 conflictCount, const PlannerInput& input)
    {
        PlannerResult result;
        if (!features || featureCount == 0 || featureCount > FeatureCount) {
            result.cycle = true;
            return result;
        }
        UInt32 requested = input.requested;
        // 冲突对在闭包前按 winner 优先消解；被拒绝项仍记录原因。
        for (UInt32 i = 0; i < conflictCount; ++i) {
            const auto loser = conflicts[i].loser, winner = conflicts[i].winner;
            if (Detail::isEnabled(requested, loser) && Detail::isEnabled(requested, winner)) {
                requested &= ~featureBit(loser);
                Detail::set(result.rejected, loser);
                Detail::set(result.conflicts, loser);
            }
        }

        constexpr UInt32 MaxFeatures = FeatureCount;
        UInt8            state[MaxFeatures]{};    // 0=未访问 1=访问中 2=已启用 3=已拒绝
        UInt32           enabled = 0;

        // 递归闭包；返回 false 时调用方标记拒绝，缺失原因由 visited 状态体现。
        struct Resolver
        {
            const PlannerFeature* features;
            UInt32                count;
            const ConflictRule*   conflicts;
            UInt32                conflictCount;
            const PlannerInput&   input;
            UInt8*                state;
            UInt32&               enabled;
            PlannerResult&        result;

            bool resolve(const FeatureId feature)
            {
                const auto index = Detail::indexOf(feature);
                if (index >= FeatureCount) { return false; }
                if (state[index] == 1) {
                    result.cycle = true;
                    return false;
                }
                if (state[index] == 2) { return true; }
                if (state[index] == 3) { return false; }
                const PlannerFeature* entry = nullptr;
                for (UInt32 i = 0; i < count; ++i) {
                    if (features[i].feature == feature) {
                        entry = &features[i];
                        break;
                    }
                }
                if (!entry) {
                    state[index] = 3;
                    Detail::set(result.rejected, feature);
                    return false;
                }
                state[index] = 1;
                if (!Detail::gated(*entry, input)) {
                    state[index] = 3;
                    Detail::set(result.rejected, feature);
                    return false;
                }
                if ((entry->external & ~input.external) != 0) {
                    state[index] = 3;
                    Detail::set(result.rejected, feature);
                    Detail::set(result.missing, feature);
                    result.missingExternal |= entry->external & ~input.external;
                    return false;
                }
                if ((input.verified & featureBit(feature)) == 0) {
                    state[index] = 3;
                    Detail::set(result.rejected, feature);
                    Detail::set(result.missing, feature);
                    return false;
                }
                if (Detail::conflictEnabled(conflicts, conflictCount, feature, enabled)) {
                    state[index] = 3;
                    Detail::set(result.rejected, feature);
                    Detail::set(result.conflicts, feature);
                    return false;
                }
                for (UInt32 i = 0; i < entry->requireCount; ++i) {
                    if (!resolve(entry->required[i])) {
                        state[index] = 3;
                        Detail::set(result.rejected, feature);
                        Detail::set(result.missing, feature);
                        return false;
                    }
                }
                state[index] = 2;
                Detail::set(enabled, feature);
                return true;
            }
        } resolver{features, featureCount, conflicts, conflictCount, input, state, enabled, result};

        for (UInt32 i = 0; i < featureCount; ++i) {
            if (Detail::isEnabled(requested, features[i].feature) && !resolver.resolve(features[i].feature)) {
                Detail::set(result.rejected, features[i].feature);
            }
        }

        result.enabled = enabled;
        // Kahn 拓扑排序：required 与 before 都产生顺序边；排序失败即报告环。
        UInt32 indegree[MaxFeatures]{};
        for (UInt32 i = 0; i < featureCount; ++i) {
            if ((enabled & featureBit(features[i].feature)) == 0) { continue; }
            const auto& entry = features[i];
            for (UInt32 r = 0; r < entry.requireCount; ++r) {
                if (Detail::isEnabled(enabled, entry.required[r])) { ++indegree[i]; }
            }
            for (UInt32 b = 0; b < entry.beforeCount; ++b) {
                if (Detail::isEnabled(enabled, entry.before[b])) { ++indegree[i]; }
            }
        }
        UInt32 remaining = enabled;
        // 用特性表的稳定顺序选择入度为 0 的节点，避免依赖哈希/遍历顺序。
        while (remaining) {
            bool progressed = false;
            for (UInt32 i = 0; i < featureCount; ++i) {
                if ((remaining & featureBit(features[i].feature)) == 0 || indegree[i] != 0) { continue; }
                result.order[result.orderCount++]  = features[i].feature;
                remaining                         &= ~featureBit(features[i].feature);
                progressed                         = true;
                // 删除该节点指向后继的边。
                for (UInt32 j = 0; j < featureCount; ++j) {
                    if ((remaining & featureBit(features[j].feature)) == 0) { continue; }
                    const auto& entry = features[j];
                    for (UInt32 r = 0; r < entry.requireCount; ++r) {
                        if (entry.required[r] == features[i].feature) { --indegree[j]; }
                    }
                    for (UInt32 b = 0; b < entry.beforeCount; ++b) {
                        if (entry.before[b] == features[i].feature) { --indegree[j]; }
                    }
                }
                break;
            }
            if (!progressed) {
                result.cycle = true;
                break;
            }
        }
        return result;
    }

    inline PlannerResult plan(const PlannerInput& input)
    {
        return evaluate(DefaultFeatures, static_cast<UInt32>(sizeof(DefaultFeatures) / sizeof(DefaultFeatures[0])),
                        nullptr, 0, input);
    }

    constexpr bool orderedBefore(const PlannerResult& result, const FeatureId first, const FeatureId second)
    {
        UInt32 firstIndex = FeatureCount, secondIndex = FeatureCount;
        for (UInt32 i = 0; i < result.orderCount; ++i) {
            if (result.order[i] == first) { firstIndex = i; }
            if (result.order[i] == second) { secondIndex = i; }
        }
        return firstIndex != FeatureCount && secondIndex != FeatureCount && firstIndex < secondIndex;
    }

}    // namespace MetalPatch::Validation

// DVPF：显示组级依赖规划；在共享 FeatureId 规划器之上提供组闭包和拒绝原因。
// 纯决策层：不读取 boot-args、不访问地址空间，安装后端必须执行准入结果。

namespace MetalPatch::Validation
{

    struct DisplayGroupSpec
    {
        DisplayGroup group;
        const char*  name;
        UInt32       features;          // 该组包含的 FeatureId 位
        UInt32       requiredGroups;    // 组级硬依赖
        UInt32       afterGroups;       // 这些组必须先于本组，只排序不构成依赖
        bool         installable;
    };

    // Metal 组由 SRD/LegacyBlend/Pipeline/Scratch/Submit 细分，SubmitGuard 独立成组。
    inline constexpr DisplayGroupSpec DefaultDisplayGroups[] = {
        {DisplayGroup::MetalRenderCore, "Metal_RenderCore",
         featureBit(FeatureId::DisplaySRD) | featureBit(FeatureId::DisplayLegacyBlend)
             | featureBit(FeatureId::DisplayPipelineConstructor),
         0, displayGroupBit(DisplayGroup::MetalSubmitSafety), true},
        {DisplayGroup::MetalScratch, "Metal_Scratch", featureBit(FeatureId::DisplayComputeScratch),
         displayGroupBit(DisplayGroup::MetalRenderCore), 0, true},
        {DisplayGroup::MetalSubmitSafety, "Metal_SubmitSafety",
         featureBit(FeatureId::DisplaySubmitGuard) | featureBit(FeatureId::DisplaySubmit), 0, 0, true},
    };

    enum class DisplayRejectReason : UInt8
    {
        None,
        UnsupportedOperatingSystem,
        UnsupportedDevice,
        MissingExternalPrerequisite,
        MissingEvidence,
        MissingDependency,
        GroupConflict,
        ExperimentalDisabled,
    };

    constexpr const char* displayRejectReasonName(const DisplayRejectReason reason)
    {
        switch (reason) {
            case DisplayRejectReason::None                       : return "None";
            case DisplayRejectReason::UnsupportedOperatingSystem : return "UnsupportedOperatingSystem";
            case DisplayRejectReason::UnsupportedDevice          : return "UnsupportedDevice";
            case DisplayRejectReason::MissingExternalPrerequisite: return "MissingExternalPrerequisite";
            case DisplayRejectReason::MissingEvidence            : return "MissingEvidence";
            case DisplayRejectReason::MissingDependency          : return "MissingDependency";
            case DisplayRejectReason::GroupConflict              : return "GroupConflict";
            case DisplayRejectReason::ExperimentalDisabled       : return "ExperimentalDisabled";
        }
        return "Unknown";
    }

    struct DisplayPlannerInput
    {
        UInt32 osMask{0};    // 单个 Darwin 位
        UInt32 deviceID{0};
        UInt32 requestedGroups{0};
        UInt32 verified{0};    // 已具备映像/完整函数/页块证据的 FeatureId
        UInt32 external{0};    // boot-arg、桥接和符号解析结果
        bool   allowExperimental{false};
    };

    struct DisplayGroupConflict
    {
        DisplayGroup loser;
        DisplayGroup winner;
    };

    struct DisplayGroupResult
    {
        UInt32              enabled{0};
        UInt32              rejected{0};
        UInt32              missing{0};
        UInt32              conflicts{0};
        DisplayGroup        order[DisplayGroupCount]{};
        UInt32              orderCount{0};
        DisplayRejectReason reasons[DisplayGroupCount]{};
        bool                cycle{false};
    };

    namespace DisplayPlannerDetail
    {

        constexpr const DisplayGroupSpec* findGroup(const DisplayGroupSpec* groups, const UInt32 count,
                                                    const DisplayGroup group)
        {
            for (UInt32 i = 0; i < count; ++i) {
                if (groups[i].group == group) { return &groups[i]; }
            }
            return nullptr;
        }

        constexpr bool applicable(const PlannerFeature& feature, const UInt32 osMask, const UInt32 deviceID)
        {
            if ((feature.osMask & osMask) == 0) { return false; }
            if (feature.deviceMask != 0 && (deviceBit(deviceID) & feature.deviceMask) == 0) { return false; }
            return true;
        }

        constexpr DisplayRejectReason featureReason(const PlannerFeature& feature, const PlannerInput& input,
                                                    const PlannerResult& result)
        {
            if ((feature.osMask & input.osMask) == 0) { return DisplayRejectReason::UnsupportedOperatingSystem; }
            if (feature.deviceMask != 0 && (deviceBit(input.deviceID) & feature.deviceMask) == 0) {
                return DisplayRejectReason::UnsupportedDevice;
            }
            if ((feature.external & ~input.external) != 0) { return DisplayRejectReason::MissingExternalPrerequisite; }
            if ((input.verified & featureBit(feature.feature)) == 0) { return DisplayRejectReason::MissingEvidence; }
            if ((result.conflicts & featureBit(feature.feature)) != 0) { return DisplayRejectReason::GroupConflict; }
            return DisplayRejectReason::MissingDependency;
        }

        constexpr UInt32 reasonRank(const DisplayRejectReason reason)
        {
            switch (reason) {
                case DisplayRejectReason::None                       : return 0;
                case DisplayRejectReason::UnsupportedOperatingSystem : return 1;
                case DisplayRejectReason::UnsupportedDevice          : return 2;
                case DisplayRejectReason::MissingExternalPrerequisite: return 3;
                case DisplayRejectReason::MissingEvidence            : return 4;
                case DisplayRejectReason::GroupConflict              : return 5;
                case DisplayRejectReason::ExperimentalDisabled       : return 6;
                case DisplayRejectReason::MissingDependency          : return 7;
            }
            return 7;
        }

        // 组闭包：请求 Scratch 会拉入 RenderCore；环直接失败闭合。
        struct GroupClosure
        {
            const DisplayGroupSpec* groups;
            UInt32                  count;
            UInt8                   state[DisplayGroupCount]{};
            UInt32                  closed{0};
            bool                    cycle{false};

            void resolve(const DisplayGroup group)
            {
                const auto index = static_cast<UInt32>(group);
                if (index >= DisplayGroupCount) { return; }
                if (state[index] == 1) {
                    cycle = true;
                    return;
                }
                if (state[index] == 2) { return; }
                const auto* spec = findGroup(groups, count, group);
                if (!spec) { return; }
                state[index] = 1;
                for (UInt32 i = 0; i < DisplayGroupCount; ++i) {
                    if ((spec->requiredGroups & displayGroupBit(static_cast<DisplayGroup>(i))) != 0) {
                        resolve(static_cast<DisplayGroup>(i));
                    }
                }
                state[index]  = 2;
                closed       |= displayGroupBit(group);
            }
        };

    }    // namespace DisplayPlannerDetail

    inline DisplayGroupResult evaluateDisplayGroups(const DisplayPlannerInput& input, const DisplayGroupSpec* groups,
                                                    const UInt32                groupCount,
                                                    const DisplayGroupConflict* conflicts     = nullptr,
                                                    const UInt32                conflictCount = 0)
    {
        using namespace DisplayPlannerDetail;
        DisplayGroupResult result;
        if (!groups || groupCount == 0 || groupCount > DisplayGroupCount) {
            result.cycle = true;
            return result;
        }

        UInt32 requested = input.requestedGroups & allDisplayGroupBits;
        // 不可安装组只在显式实验模式下进入闭包；默认组不得被无意启用。
        for (UInt32 i = 0; i < groupCount; ++i) {
            const auto bit = displayGroupBit(groups[i].group);
            if ((requested & bit) != 0 && !groups[i].installable && !input.allowExperimental) {
                requested                                            &= ~bit;
                result.rejected                                      |= bit;
                result.reasons[static_cast<UInt32>(groups[i].group)]  = DisplayRejectReason::ExperimentalDisabled;
            }
        }

        GroupClosure closure{groups, groupCount};
        for (UInt32 i = 0; i < groupCount; ++i) {
            if ((requested & displayGroupBit(groups[i].group)) != 0) { closure.resolve(groups[i].group); }
        }
        if (closure.cycle) {
            result.cycle = true;
            return result;
        }

        const UInt32 unresolved = requested & ~closure.closed;
        if (unresolved != 0) {
            result.rejected |= unresolved;
            result.missing  |= unresolved;
            for (UInt32 i = 0; i < groupCount; ++i) {
                const auto bit = displayGroupBit(groups[i].group);
                if ((unresolved & bit) != 0) {
                    result.reasons[static_cast<UInt32>(groups[i].group)] = DisplayRejectReason::MissingDependency;
                }
            }
        }

        // 组冲突在请求闭包之后消解；依赖被拒绝组的其它组会在下面的依赖检查中失败闭合。
        for (UInt32 i = 0; conflicts && i < conflictCount; ++i) {
            const auto loser  = displayGroupBit(conflicts[i].loser);
            const auto winner = displayGroupBit(conflicts[i].winner);
            if ((closure.closed & loser) != 0 && (closure.closed & winner) != 0) {
                closure.closed                                          &= ~loser;
                result.rejected                                         |= loser;
                result.conflicts                                        |= loser;
                result.reasons[static_cast<UInt32>(conflicts[i].loser)]  = DisplayRejectReason::GroupConflict;
            }
        }

        UInt32 requestedFeatures = 0;
        for (UInt32 i = 0; i < groupCount; ++i) {
            const auto bit = displayGroupBit(groups[i].group);
            if ((closure.closed & bit) == 0) { continue; }
            for (UInt32 f = 0; f < FeatureCount; ++f) {
                const auto feature = static_cast<FeatureId>(f);
                if ((groups[i].features & featureBit(feature)) == 0) { continue; }
                const auto* spec = findFeature(DefaultFeatures, FeatureCount, feature);
                if (spec && applicable(*spec, input.osMask, input.deviceID)) {
                    requestedFeatures |= featureBit(feature);
                }
            }
        }

        PlannerInput featureInput{input.osMask, input.deviceID, requestedFeatures, input.verified, input.external};
        const auto   featureResult = plan(featureInput);

        for (UInt32 i = 0; i < groupCount; ++i) {
            const auto group = groups[i].group;
            const auto index = static_cast<UInt32>(group);
            const auto bit   = displayGroupBit(group);
            if ((closure.closed & bit) == 0) { continue; }
            UInt32              applicableCount = 0, enabledCount = 0;
            DisplayRejectReason worst = DisplayRejectReason::None;
            for (UInt32 f = 0; f < FeatureCount; ++f) {
                const auto feature = static_cast<FeatureId>(f);
                if ((groups[i].features & featureBit(feature)) == 0) { continue; }
                const auto* spec = findFeature(DefaultFeatures, FeatureCount, feature);
                if (!spec || !applicable(*spec, input.osMask, input.deviceID)) { continue; }
                ++applicableCount;
                if ((featureResult.enabled & featureBit(feature)) != 0) {
                    ++enabledCount;
                    continue;
                }
                const auto reason = featureReason(*spec, featureInput, featureResult);
                if (worst == DisplayRejectReason::None || reasonRank(reason) < reasonRank(worst)) { worst = reason; }
            }
            if (applicableCount == 0) {
                bool osMatch = false;
                for (UInt32 f = 0; f < FeatureCount; ++f) {
                    const auto feature = static_cast<FeatureId>(f);
                    if ((groups[i].features & featureBit(feature)) == 0) { continue; }
                    const auto* spec = findFeature(DefaultFeatures, FeatureCount, feature);
                    if (spec && (spec->osMask & input.osMask) != 0) { osMatch = true; }
                }
                result.rejected |= bit;
                result.reasons[index] =
                    osMatch ? DisplayRejectReason::UnsupportedDevice : DisplayRejectReason::UnsupportedOperatingSystem;
                continue;
            }
            if (enabledCount == applicableCount) {
                result.enabled |= bit;
                continue;
            }
            result.rejected |= bit;
            result.reasons[index] = worst == DisplayRejectReason::None ? DisplayRejectReason::MissingDependency : worst;
            if (worst == DisplayRejectReason::MissingEvidence) { result.missing |= bit; }
            if (worst == DisplayRejectReason::GroupConflict) { result.conflicts |= bit; }
        }

        // 组级硬依赖：被依赖组未启用时，依赖组必须失败闭合（可能发生多轮）。
        bool changed = true;
        while (changed) {
            changed = false;
            for (UInt32 i = 0; i < groupCount; ++i) {
                const auto bit = displayGroupBit(groups[i].group);
                if ((result.enabled & bit) == 0) { continue; }
                const auto required = groups[i].requiredGroups;
                if (required == 0 || (required & ~result.enabled) == 0) { continue; }
                result.enabled                                       &= ~bit;
                result.rejected                                      |= bit;
                result.missing                                       |= bit;
                result.reasons[static_cast<UInt32>(groups[i].group)]  = DisplayRejectReason::MissingDependency;
                changed                                               = true;
            }
        }

        // Kahn 拓扑排序：required 与 before 都产生边；排序失败报告环而不是部分成功。
        UInt32 indegree[DisplayGroupCount]{};
        for (UInt32 i = 0; i < groupCount; ++i) {
            const auto bit = displayGroupBit(groups[i].group);
            if ((result.enabled & bit) == 0) { continue; }
            const auto edges = groups[i].requiredGroups | groups[i].afterGroups;
            for (UInt32 j = 0; j < groupCount; ++j) {
                if (i == j) { continue; }
                const auto other = displayGroupBit(groups[j].group);
                if ((edges & other) != 0 && (result.enabled & other) != 0) { ++indegree[i]; }
            }
        }
        UInt32 remaining = result.enabled;
        while (remaining) {
            bool progressed = false;
            for (UInt32 i = 0; i < groupCount; ++i) {
                const auto bit = displayGroupBit(groups[i].group);
                if ((remaining & bit) == 0 || indegree[i] != 0) { continue; }
                result.order[result.orderCount++]  = groups[i].group;
                remaining                         &= ~bit;
                progressed                         = true;
                for (UInt32 j = 0; j < groupCount; ++j) {
                    if (i == j || (remaining & displayGroupBit(groups[j].group)) == 0) { continue; }
                    if ((groups[j].requiredGroups & bit) != 0) { --indegree[j]; }
                    if ((groups[j].afterGroups & bit) != 0) { --indegree[j]; }
                }
                break;
            }
            if (!progressed) {
                result.cycle = true;
                break;
            }
        }
        return result;
    }

    inline DisplayGroupResult planDisplay(const DisplayPlannerInput& input)
    {
        return evaluateDisplayGroups(
            input, DefaultDisplayGroups,
            static_cast<UInt32>(sizeof(DefaultDisplayGroups) / sizeof(DefaultDisplayGroups[0])));
    }

    constexpr bool orderedBefore(const DisplayGroupResult& result, const DisplayGroup first, const DisplayGroup second)
    {
        UInt32 firstIndex = DisplayGroupCount, secondIndex = DisplayGroupCount;
        for (UInt32 i = 0; i < result.orderCount; ++i) {
            if (result.order[i] == first) { firstIndex = i; }
            if (result.order[i] == second) { secondIndex = i; }
        }
        return firstIndex != DisplayGroupCount && secondIndex != DisplayGroupCount && firstIndex < secondIndex;
    }

}    // namespace MetalPatch::Validation

// DVPF：共享 CapabilityLedger 的显示组视图；不另建第二套地址空间账本。

namespace MetalPatch::Validation
{

    // HardwareValidated 只能由实机报告给出，运行时状态机不产生该值。
    enum class DisplayGroupState : UInt8
    {
        Disabled,
        Candidate,
        Resolved,
        Verified,
        InstallEligible,
        Installing,
        Installed,
        RuntimeObserved,
        CommitFailed,
        Rejected,
        Unsupported,
        RollbackComplete,
        RollbackFailed,
        Quarantined,
    };

    constexpr const char* displayGroupStateName(const DisplayGroupState state)
    {
        switch (state) {
            case DisplayGroupState::Disabled        : return "Disabled";
            case DisplayGroupState::Candidate       : return "Candidate";
            case DisplayGroupState::Resolved        : return "Resolved";
            case DisplayGroupState::Verified        : return "Verified";
            case DisplayGroupState::InstallEligible : return "InstallEligible";
            case DisplayGroupState::Installing      : return "Installing";
            case DisplayGroupState::Installed       : return "Installed";
            case DisplayGroupState::RuntimeObserved : return "RuntimeObserved";
            case DisplayGroupState::CommitFailed    : return "CommitFailed";
            case DisplayGroupState::Rejected        : return "Rejected";
            case DisplayGroupState::Unsupported     : return "Unsupported";
            case DisplayGroupState::RollbackComplete: return "RollbackComplete";
            case DisplayGroupState::RollbackFailed  : return "RollbackFailed";
            case DisplayGroupState::Quarantined     : return "Quarantined";
        }
        return "Unknown";
    }

    namespace DisplayStateDetail
    {

        // 失败/不支持状态不参与“全部达到某阶段”的判断。
        constexpr UInt32 progress(const CapabilityState state)
        {
            switch (state) {
                case CapabilityState::Disabled       : return 0;
                case CapabilityState::Eligible       : return 1;
                case CapabilityState::ImageResolved  : return 2;
                case CapabilityState::Verified       : return 3;
                case CapabilityState::Prepared       : return 4;
                case CapabilityState::Installing     : return 5;
                case CapabilityState::Installed      : return 6;
                case CapabilityState::RuntimeObserved: return 7;
                default                              : return 0;
            }
        }

    }    // namespace DisplayStateDetail

    // features 限定当前 OS/设备适用的成员；缺少证据的适用成员仍参与聚合。
    template<UInt32 Capacity>
    inline DisplayGroupState displayGroupState(const CapabilityLedgerStorage<Capacity>& ledger, const UInt64 map,
                                               const DisplayGroup group, const UInt32 features = allFeatureBits)
    {
        const auto* spec = DisplayPlannerDetail::findGroup(
            DefaultDisplayGroups, static_cast<UInt32>(sizeof(DefaultDisplayGroups) / sizeof(DefaultDisplayGroups[0])),
            group);
        if (!spec) { return DisplayGroupState::Disabled; }

        bool any = false, allDisabled = true;
        bool quarantined = false, rollbackFailed = false, rollbackVerified = false;
        bool unsupported = false, rejected = false, commitFailed = false, installing = false;
        bool anyInstalled = false, allInstalled = true, anyRuntime = false;
        bool allPrepared = true, allVerified = true, allResolved = true;

        for (UInt32 i = 0; i < FeatureCount; ++i) {
            const auto feature = static_cast<FeatureId>(i);
            if ((spec->features & features & featureBit(feature)) == 0) { continue; }
            any              = true;
            const auto state = ledger.state(map, feature);
            if (state != CapabilityState::Disabled) { allDisabled = false; }
            quarantined          |= state == CapabilityState::Quarantined;
            rollbackFailed       |= state == CapabilityState::RollbackFailed;
            rollbackVerified     |= state == CapabilityState::RollbackVerified;
            unsupported          |= state == CapabilityState::Unsupported;
            rejected             |= state == CapabilityState::Rejected;
            commitFailed         |= state == CapabilityState::CommitFailed;
            installing           |= state == CapabilityState::Installing;
            const auto stage      = DisplayStateDetail::progress(state);
            const bool installed  = stage >= DisplayStateDetail::progress(CapabilityState::Installed);
            anyInstalled         |= installed;
            allInstalled         &= installed;
            anyRuntime           |= stage >= DisplayStateDetail::progress(CapabilityState::RuntimeObserved);
            allPrepared          &= stage >= DisplayStateDetail::progress(CapabilityState::Prepared);
            allVerified          &= stage >= DisplayStateDetail::progress(CapabilityState::Verified);
            allResolved          &= stage >= DisplayStateDetail::progress(CapabilityState::ImageResolved);
        }

        if (!any || allDisabled) { return DisplayGroupState::Disabled; }
        if (quarantined) { return DisplayGroupState::Quarantined; }
        if (rollbackFailed) { return DisplayGroupState::RollbackFailed; }
        if (rollbackVerified) { return DisplayGroupState::RollbackComplete; }
        if (unsupported) { return DisplayGroupState::Unsupported; }
        if (rejected) { return DisplayGroupState::Rejected; }
        if (commitFailed) { return DisplayGroupState::CommitFailed; }
        if (installing) { return DisplayGroupState::Installing; }
        if (anyRuntime && allInstalled) { return DisplayGroupState::RuntimeObserved; }
        if (anyInstalled && allInstalled) { return DisplayGroupState::Installed; }
        if (allPrepared) { return DisplayGroupState::InstallEligible; }
        if (allVerified) { return DisplayGroupState::Verified; }
        if (allResolved) { return DisplayGroupState::Resolved; }
        return DisplayGroupState::Candidate;
    }

    // 安装门与诊断共享同一份 FeatureId 账本。
    template<UInt32 Capacity>
    inline bool markDisplayGroup(CapabilityLedgerStorage<Capacity>& ledger, const UInt64 map, const DisplayGroup group,
                                 const CapabilityState state, const UInt32 features = allFeatureBits)
    {
        const auto* spec = DisplayPlannerDetail::findGroup(
            DefaultDisplayGroups, static_cast<UInt32>(sizeof(DefaultDisplayGroups) / sizeof(DefaultDisplayGroups[0])),
            group);
        if (!spec) { return false; }
        bool ok = true;
        for (UInt32 i = 0; i < FeatureCount; ++i) {
            const auto feature = static_cast<FeatureId>(i);
            if ((spec->features & features & featureBit(feature)) == 0) { continue; }
            ok &= ledger.set(map, feature, state);
        }
        return ok;
    }

}    // namespace MetalPatch::Validation

// 每次页交付/共享缓存事务的显示准入；证据和状态不跨调用、进程或地址空间复用。

namespace MetalPatch::Validation
{

    constexpr UInt32 MetalDisplayGroups = displayGroupBit(DisplayGroup::MetalRenderCore)
                                          | displayGroupBit(DisplayGroup::MetalScratch)
                                          | displayGroupBit(DisplayGroup::MetalSubmitSafety);

    inline UInt32 displayFeaturesForGroups(const UInt32 groups, const UInt32 osMask, const UInt32 deviceID)
    {
        UInt32 features = 0;
        for (const auto& group : DefaultDisplayGroups) {
            if ((groups & displayGroupBit(group.group)) == 0) { continue; }
            for (const auto& feature : DefaultFeatures) {
                if ((group.features & featureBit(feature.feature)) != 0
                    && DisplayPlannerDetail::applicable(feature, osMask, deviceID))
                {
                    features |= featureBit(feature.feature);
                }
            }
        }
        return features;
    }

    template<UInt32 Capacity = FeatureCount>
    class DisplayRuntime
    {
        CapabilityLedgerStorage<Capacity> ledger_{};
        DisplayPlannerInput               input_{};
        DisplayGroupResult                result_{};
        UInt64                            map_{0};
        UInt32                            features_{0};
        bool                              started_{false};

    public:
        DisplayRuntime()                                 = default;
        DisplayRuntime(const DisplayRuntime&)            = delete;
        DisplayRuntime& operator=(const DisplayRuntime&) = delete;

        __attribute__((noinline)) void reset(const UInt64 map, const UInt32 major, const UInt32 deviceID,
                                             const UInt32 requestedGroups)
        {
            ledger_.clear();
            map_   = map;
            input_ = {displayOsMask(major), deviceID, requestedGroups, 0, 0, false};
            DisplayPlannerDetail::GroupClosure closure{DefaultDisplayGroups,
                                                       sizeof(DefaultDisplayGroups) / sizeof(DefaultDisplayGroups[0])};
            for (UInt32 i = 0; i < DisplayGroupCount; ++i) {
                if ((requestedGroups & displayGroupBit(static_cast<DisplayGroup>(i))) != 0) {
                    closure.resolve(static_cast<DisplayGroup>(i));
                }
            }
            features_ = displayFeaturesForGroups(closure.closed, input_.osMask, deviceID);
            result_   = {};
            started_  = false;
        }

        // 只在对应后端完成完整计划/入口读回后提供证据。
        __attribute__((noinline)) bool verify(const UInt32 features, const UInt32 external)
        {
            if (started_) { return false; }
            input_.verified |= features & features_;
            input_.external |= external;
            result_          = evaluateDisplayGroups(input_, DefaultDisplayGroups,
                                                     sizeof(DefaultDisplayGroups) / sizeof(DefaultDisplayGroups[0]));
            return !result_.cycle && input_.requestedGroups != 0
                   && (result_.enabled & input_.requestedGroups) == input_.requestedGroups;
        }

        __attribute__((noinline)) bool begin()
        {
            if (started_ || !map_ || result_.cycle || input_.requestedGroups == 0
                || (result_.enabled & input_.requestedGroups) != input_.requestedGroups)
            {
                return false;
            }
            // 安装前按规划器顺序推进；同一 Metal 原子事务可涵盖多个组。
            for (UInt32 i = 0; i < result_.orderCount; ++i) {
                const auto                group    = result_.order[i];
                constexpr CapabilityState stages[] = {CapabilityState::Eligible, CapabilityState::ImageResolved,
                                                      CapabilityState::Verified, CapabilityState::Prepared};
                for (const auto state : stages) {
                    if (!markDisplayGroup(ledger_, map_, group, state, features_)) { return false; }
                }
                if (state(group) != DisplayGroupState::InstallEligible) { return false; }
                if (!markDisplayGroup(ledger_, map_, group, CapabilityState::Installing, features_)) { return false; }
            }
            started_ = true;
            return true;
        }

        bool installed(const UInt32 features)
        {
            if (!started_) { return false; }
            bool ok = true;
            for (UInt32 i = 0; i < FeatureCount; ++i) {
                const auto feature = static_cast<FeatureId>(i);
                if ((features & features_ & featureBit(feature)) != 0) {
                    ok &= ledger_.set(map_, feature, CapabilityState::Installed);
                }
            }
            return ok;
        }

        void failed(const bool rollbackOk)
        {
            for (UInt32 i = 0; i < FeatureCount; ++i) {
                const auto feature = static_cast<FeatureId>(i);
                if (ledger_.state(map_, feature) != CapabilityState::Installing) { continue; }
                ledger_.set(map_, feature, CapabilityState::CommitFailed);
                ledger_.set(map_, feature,
                            rollbackOk ? CapabilityState::RollbackVerified : CapabilityState::RollbackFailed);
                if (!rollbackOk) { ledger_.set(map_, feature, CapabilityState::Quarantined); }
            }
        }

        DisplayGroupState state(const DisplayGroup group) const
        { return displayGroupState(ledger_, map_, group, features_); }
        bool ready() const
        {
            if (!started_) { return false; }
            for (UInt32 i = 0; i < result_.orderCount; ++i) {
                const auto current = state(result_.order[i]);
                if (current != DisplayGroupState::Installing && current != DisplayGroupState::Installed) {
                    return false;
                }
            }
            return true;
        }
        UInt32                    features() const { return features_; }
        const DisplayGroupResult& result() const { return result_; }
    };

}    // namespace MetalPatch::Validation
