#pragma once
#include <IOKit/IOTypes.h>
#include <MetalPatch/PatchPlan.hpp>
#include <stddef.h>

// MetalPatch::Validation：共享缓存页面事务的统一执行器；只编排已核验计划，不解析、不选择功能。

namespace MetalPatch::Validation
{

    enum class TransactionStatus : UInt8
    {
        Installed,
        CheckFailed,
        BeforeWriteFailed,
        ProtectionFailed,
        WriteFailed,
        ReadbackFailed,
        RestoreFailed,
    };

    // Ops 由调用方提供：check/makeWritable/prepareRollback/restore/write。
    // 执行器不直接依赖 vm_map/mach API，便于内核与用户态夹具共用同一实现。
    template<typename Ops, typename Patch, typename Page, UInt32 MaxPatchSize>
    class SharedRegionTransaction
    {
        Ops&         ops_;
        UInt64       base_;
        const Patch* patches_;
        UInt32       count_;
        UInt8        (*saved_)[MaxPatchSize];
        Page*        pages_;
        UInt32       pageCount_;
        UInt32       protectedPrefix_;

        bool rollback(const UInt32 touched)
        {
            bool ok = true;
            for (UInt32 remaining = touched; remaining > 0; --remaining) {
                const UInt32 index = remaining - 1;
                // 守卫前缀回滚失败后保留，避免把仍在使用的安全入口撤掉。
                if (!ok && index < protectedPrefix_) { continue; }
                const UInt64 address = base_ + patches_[index].offset;
                if (!ops_.prepareRollback(pages_, pageCount_, address, patches_[index].size)) {
                    ok = false;
                    continue;
                }
                if (ops_.write(saved_[index], address, patches_[index].size) != 0) { ok = false; }
            }
            if (!ops_.restore(pages_, pageCount_)) { ok = false; }
            return ok;
        }

    public:
        struct Result
        {
            TransactionStatus status{TransactionStatus::CheckFailed};
            UInt32            touched{0};
            bool              rollbackOk{false};
            bool              restoreOk{false};
        };

        SharedRegionTransaction(Ops& ops, const UInt64 base, const Patch* patches, const UInt32 count,
                                UInt8 (*saved)[MaxPatchSize], Page* pages, const UInt32 pageCount,
                                const UInt32 protectedPrefix) :
            ops_(ops),
            base_(base),
            patches_(patches),
            count_(count),
            saved_(saved),
            pages_(pages),
            pageCount_(pageCount),
            protectedPrefix_(protectedPrefix)
        { }

        // 顺序：权限检查 -> 写前回调（如创建守卫页）-> COW 可写 -> 写 -> 分块读回
        // -> 恢复 RX -> 必要时反向回滚。失败时返回真正修改过的前缀，供调用方保留守卫页。
        template<typename BeforeWrite>
        Result commit(BeforeWrite beforeWrite)
        {
            if (!patches_ || !saved_ || count_ == 0 || (pageCount_ != 0 && !pages_)) {
                return {TransactionStatus::CheckFailed, 0, false, false};
            }
            for (UInt32 i = 0; i < pageCount_; ++i) {
                if (!ops_.check(pages_[i])) { return {TransactionStatus::CheckFailed, 0, false, false}; }
            }
            if (!beforeWrite()) { return {TransactionStatus::BeforeWriteFailed, 0, false, false}; }
            for (UInt32 i = 0; i < pageCount_; ++i) {
                if (!ops_.makeWritable(pages_[i])) {
                    const bool restoreOk = ops_.restore(pages_, pageCount_);
                    return {TransactionStatus::ProtectionFailed, 0, false, restoreOk};
                }
            }
            for (UInt32 i = 0; i < count_; ++i) {
                if (ops_.write(patches_[i].patched, base_ + patches_[i].offset, patches_[i].size) != 0) {
                    const bool rollbackOk = rollback(i + 1);
                    return {TransactionStatus::WriteFailed, i + 1, rollbackOk, true};
                }
            }
            // 64 字节分块读回，避免为 448/452 字节补丁增加大栈帧。
            UInt8 chunk[64];
            for (UInt32 i = 0; i < count_; ++i) {
                const UInt8* expected = patches_[i].patched;
                for (UInt32 offset = 0; offset < patches_[i].size;) {
                    const UInt32 length = patches_[i].size - offset < sizeof(chunk) ?
                                              patches_[i].size - offset :
                                              static_cast<UInt32>(sizeof(chunk));
                    if (ops_.read(base_ + patches_[i].offset + offset, chunk, length) != 0) {
                        const bool rollbackOk = rollback(count_);
                        return {TransactionStatus::ReadbackFailed, count_, rollbackOk, true};
                    }
                    for (UInt32 j = 0; j < length; ++j) {
                        if (chunk[j] != expected[offset + j]) {
                            const bool rollbackOk = rollback(count_);
                            return {TransactionStatus::ReadbackFailed, count_, rollbackOk, true};
                        }
                    }
                    offset += length;
                }
            }
            if (!ops_.restore(pages_, pageCount_)) {
                const bool rollbackOk = rollback(count_);
                return {TransactionStatus::RestoreFailed, count_, rollbackOk, false};
            }
            return {TransactionStatus::Installed, count_, true, true};
        }
    };

}    // namespace MetalPatch::Validation

// DVPF：Metal 共享缓存计划执行器；只接收已验证/可读回核验的计划。
// 页面收集、COW、W^X、读回、回滚和权限恢复继续由 MetalPatch::Memory 公共实现完成。

namespace MetalPatch::Validation
{

    // 把显示解析器实际选出的补丁装入计划；站点字节仍由执行器安装前读回核验。
    // 不绑定视频 FeatureManifest，调用方必须在读回后 seal(PlanState)。
    template<typename Patch>
    inline bool adaptDisplayPatches(VerifiedPatchPlan& out, const UInt64 base,
                                    const MetalPatch::SharedCache::Range& cache, const Patch* patches,
                                    const UInt32 patchCount)
    {
        if (!patches || patchCount == 0 || patchCount > MaxPlanSites || base == 0 || cache.size == 0) { return false; }
        out.reset(base, cache);
        for (UInt32 i = 0; i < patchCount; ++i) {
            const auto& patch = patches[i];
            if (patch.size == 0 || patch.size > MaxPlanPatchSize
                || patch.offset >= MetalPatch::SharedCache::UserLimit - base
                || patch.size >= MetalPatch::SharedCache::UserLimit - base - patch.offset)
            {
                return false;
            }
            if (!out.addSite(base + patch.offset, patch.offset, patch.size, NoFunction, 0, patch.original,
                             patch.patched))
            {
                return false;
            }
        }
        return true;
    }

    enum class MetalPrepareState : UInt8
    {
        Invalid,
        AllOriginal,
        AllPatched,
        Mixed,
    };

    enum class MetalPrepareFault : UInt8
    {
        None,
        InvalidPlan,
        ReadFailed,
        Mismatch,
        PageLimit,
        SealFailed,
    };

    struct MetalPrepareResult
    {
        MetalPrepareState state{MetalPrepareState::Invalid};
        MetalPrepareFault fault{MetalPrepareFault::None};
        UInt32            pageCount{0};
        UInt32            originals{0};
        UInt32            patched{0};
    };

    struct MetalPatchView
    {
        UInt64       offset;
        const UInt8* original;
        const UInt8* patched;
        UInt32       size;
    };

    // MaxPages 必须由调用方按每次事务的实际栈/堆预算给出；不在这里分配页面。
    template<typename Ops, typename Page, UInt32 MaxPatchSize, UInt32 MaxPages, UInt32 MaxSites = MaxPlanSites>
    class MetalTransaction
    {
        Ops&               ops_;
        VerifiedPatchPlan& plan_;
        UInt64             base_;
        UInt8              (*saved_)[MaxPatchSize];
        Page               (&pages_)[MaxPages];
        MetalPatchView     patches_[MaxSites]{};
        UInt32             count_{0};
        UInt32             pageCount_{0};
        MetalPrepareState  state_{MetalPrepareState::Invalid};
        bool               prepared_{false};

    public:
        MetalTransaction(Ops& ops, VerifiedPatchPlan& plan, const UInt64 base, UInt8 (*saved)[MaxPatchSize],
                         Page (&pages)[MaxPages]) :
            ops_(ops),
            plan_(plan),
            base_(base),
            saved_(saved),
            pages_(pages)
        { }

        MetalTransaction(const MetalTransaction&)            = delete;
        MetalTransaction& operator=(const MetalTransaction&) = delete;

        // 读取每个站点并按 original/replacement 判定状态；全部读取、页面收集和
        // 计划封存成功后才允许进入写阶段。
        bool prepare(MetalPrepareResult& result)
        {
            result     = {};
            count_     = 0;
            pageCount_ = 0;
            state_     = MetalPrepareState::Invalid;
            prepared_  = false;
            count_     = plan_.siteCount();
            if (!saved_ || count_ == 0 || count_ > MaxSites || base_ == 0) {
                result.fault = MetalPrepareFault::InvalidPlan;
                return false;
            }
            UInt32 originals{0}, patched{0};
            for (UInt32 i = 0; i < count_; ++i) {
                const auto& site = plan_.site(i);
                if (site.size == 0 || site.size > MaxPatchSize || site.address < base_
                    || site.address - base_ != site.offset || site.address >= MetalPatch::SharedCache::UserLimit
                    || site.size > MetalPatch::SharedCache::UserLimit - site.address)
                {
                    result.fault = MetalPrepareFault::InvalidPlan;
                    return false;
                }
                if (ops_.read(site.address, saved_[i], site.size) != 0) {
                    result.fault = MetalPrepareFault::ReadFailed;
                    return false;
                }
                if (memcmp(saved_[i], site.original, site.size) == 0) { ++originals; }
                else if (memcmp(saved_[i], site.replacement, site.size) == 0) {
                    ++patched;
                }
                else {
                    result.fault = MetalPrepareFault::Mismatch;
                    return false;
                }
                if (!ops_.collectPages(pages_, pageCount_, site.address, site.size)) {
                    result.fault = MetalPrepareFault::PageLimit;
                    return false;
                }
                patches_[i] = {site.offset, site.original, site.replacement, site.size};
            }
            if (originals == count_) { state_ = MetalPrepareState::AllOriginal; }
            else if (patched == count_) {
                state_ = MetalPrepareState::AllPatched;
            }
            else {
                state_ = MetalPrepareState::Mixed;
            }

            const auto planState =
                state_ == MetalPrepareState::AllOriginal ?
                    PlanState::AllOriginal :
                    (state_ == MetalPrepareState::AllPatched ? PlanState::AllPatched : PlanState::Mixed);
            if (!plan_.seal(planState)) {
                result.fault = MetalPrepareFault::SealFailed;
                return false;
            }
            prepared_        = true;
            result.state     = state_;
            result.pageCount = pageCount_;
            result.originals = originals;
            result.patched   = patched;
            return true;
        }

        // 仅 AllOriginal 进入写入；AllPatched/Mixed 由调用方按幂等或拒绝路径处理。
        template<typename BeforeWrite>
        typename SharedRegionTransaction<Ops, MetalPatchView, Page, MaxPatchSize>::Result
            commit(BeforeWrite beforeWrite)
        {
            using Executor = SharedRegionTransaction<Ops, MetalPatchView, Page, MaxPatchSize>;
            if (!prepared_ || state_ != MetalPrepareState::AllOriginal) { return {}; }
            Executor executor(ops_, base_, patches_, count_, saved_, pages_, pageCount_, 0);
            return executor.commit(beforeWrite);
        }

        MetalPrepareState        state() const { return state_; }
        UInt32                   siteCount() const { return count_; }
        UInt32                   pageCount() const { return pageCount_; }
        const VerifiedPatchPlan& plan() const { return plan_; }
    };

}    // namespace MetalPatch::Validation

// DVPF：SubmitGuard 等可执行跳板的显式生命周期；入口仍可能引用时禁止释放页面。

namespace MetalPatch::Validation
{

    enum class TrampolineState : UInt8
    {
        Empty,
        Allocated,
        Filled,
        Executable,
        EntryLinked,
        Verified,
        Retained,
        Released,
    };

    enum class TrampolineFault : UInt8
    {
        None,
        InvalidTransition,
        AllocationFailed,
        FillFailed,
        ProtectionFailed,
        EntryLinkFailed,
        EntryVerifyFailed,
        EntryRestoreFailed,
        ReleaseRejected,
    };

    struct TrampolineRecord
    {
        UInt64          page{0};
        UInt64          entry{0};
        UInt32          payloadSize{0};
        TrampolineState state{TrampolineState::Empty};
        bool            entryLinked{false};
        bool            pageRetained{false};
        TrampolineFault fault{TrampolineFault::None};
    };

    constexpr const char* trampolineStateName(const TrampolineState state)
    {
        switch (state) {
            case TrampolineState::Empty      : return "Empty";
            case TrampolineState::Allocated  : return "Allocated";
            case TrampolineState::Filled     : return "Filled";
            case TrampolineState::Executable : return "Executable";
            case TrampolineState::EntryLinked: return "EntryLinked";
            case TrampolineState::Verified   : return "Verified";
            case TrampolineState::Retained   : return "Retained";
            case TrampolineState::Released   : return "Released";
        }
        return "Unknown";
    }

    // 纯状态机：不分配内存、不写页；真实 VM 操作由既有安装器执行。
    class ExecutableTrampolineResource
    {
        TrampolineRecord record_{};

        bool invalid(const TrampolineFault fault)
        {
            record_.fault = fault;
            return false;
        }

    public:
        const TrampolineRecord& record() const { return record_; }

        bool allocate(const UInt64 page)
        {
            if (record_.state != TrampolineState::Empty || page == 0) {
                return invalid(TrampolineFault::AllocationFailed);
            }
            record_.page         = page;
            record_.state        = TrampolineState::Allocated;
            record_.entryLinked  = false;
            record_.pageRetained = false;
            record_.fault        = TrampolineFault::None;
            return true;
        }

        bool fill(const UInt32 payloadSize)
        {
            if (record_.state != TrampolineState::Allocated) { return invalid(TrampolineFault::FillFailed); }
            record_.payloadSize = payloadSize;
            record_.state       = TrampolineState::Filled;
            record_.fault       = TrampolineFault::None;
            return true;
        }

        bool makeExecutable()
        {
            if (record_.state != TrampolineState::Filled) { return invalid(TrampolineFault::ProtectionFailed); }
            record_.state = TrampolineState::Executable;
            record_.fault = TrampolineFault::None;
            return true;
        }

        bool linkEntry(const UInt64 entry)
        {
            if (record_.state != TrampolineState::Executable || entry == 0) {
                return invalid(TrampolineFault::EntryLinkFailed);
            }
            record_.entry       = entry;
            record_.entryLinked = true;
            record_.state       = TrampolineState::EntryLinked;
            record_.fault       = TrampolineFault::None;
            return true;
        }

        // 读回载荷/入口一致后才算 Verified；否则保留页面等待外部恢复入口。
        bool verify(const bool entryMatches)
        {
            if (record_.state != TrampolineState::EntryLinked) { return invalid(TrampolineFault::EntryVerifyFailed); }
            if (!entryMatches) {
                record_.state        = TrampolineState::Retained;
                record_.pageRetained = true;
                return invalid(TrampolineFault::EntryVerifyFailed);
            }
            record_.state = TrampolineState::Verified;
            record_.fault = TrampolineFault::None;
            return true;
        }

        // 回滚或卸载时确认入口已恢复；未恢复则保留页面，不自动重试。
        bool restoreEntry(const bool restored)
        {
            if (record_.state != TrampolineState::EntryLinked && record_.state != TrampolineState::Retained
                && record_.state != TrampolineState::Verified)
            {
                return invalid(TrampolineFault::EntryRestoreFailed);
            }
            if (!restored) {
                record_.entryLinked  = true;
                record_.pageRetained = true;
                record_.state        = TrampolineState::Retained;
                return invalid(TrampolineFault::EntryRestoreFailed);
            }
            record_.entryLinked  = false;
            record_.pageRetained = false;
            record_.state        = TrampolineState::Verified;
            record_.fault        = TrampolineFault::None;
            return true;
        }

        // 只有入口未链接或已确认恢复时才允许释放；Retained 不得直接释放。
        bool release()
        {
            if (record_.state == TrampolineState::Released) { return true; }
            if (record_.state == TrampolineState::EntryLinked || record_.state == TrampolineState::Retained
                || record_.entryLinked)
            {
                return invalid(TrampolineFault::ReleaseRejected);
            }
            if (record_.state != TrampolineState::Allocated && record_.state != TrampolineState::Filled
                && record_.state != TrampolineState::Executable && record_.state != TrampolineState::Verified)
            {
                return invalid(TrampolineFault::ReleaseRejected);
            }
            record_.state        = TrampolineState::Released;
            record_.pageRetained = false;
            record_.entryLinked  = false;
            record_.fault        = TrampolineFault::None;
            return true;
        }

        TrampolineRecord rollback(const bool entryRestored)
        {
            if (record_.state == TrampolineState::Empty || record_.state == TrampolineState::Released) {
                return record_;
            }
            if (record_.state == TrampolineState::EntryLinked || record_.state == TrampolineState::Retained) {
                if (!restoreEntry(entryRestored)) { return record_; }
            }
            release();
            return record_;
        }

        bool linked() const
        {
            return record_.entryLinked
                   && (record_.state == TrampolineState::EntryLinked || record_.state == TrampolineState::Verified
                       || record_.state == TrampolineState::Retained);
        }

        bool pageRetained() const { return record_.pageRetained; }

        // 测试用不变式：Released 不得同时保留入口跳转。
        bool invariant() const { return !(record_.state == TrampolineState::Released && record_.entryLinked); }
    };

}    // namespace MetalPatch::Validation
