#pragma once

#ifndef _3D_DISABLED
#include "core/object/ref_counted.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/semaphore.h"
#include "core/templates/hash_map.h"
#include "scene/animation/animation_tree.h"
#include "scene/animation/animation_player.h"
#include "scene/3d/skeleton_animation_pose.h"
#include <atomic>

// Each registered live AnimationMixer/AnimationTree runtime is exclusive to its worker
// until complete() joins it. complete() only waits; publish(handle) publishes one entry;
// complete_and_publish() publishes the remaining entries for the frame. Publication and
// scene-tree/event writes stay on the main thread. Global compatibility hooks finish or
// fall back when shared Resources or target nodes create indirect dependencies.
class AnimationBatchProcessor : public RefCounted {
    GDCLASS(AnimationBatchProcessor, RefCounted);
    friend class AnimationBatchMutationScope;
    struct Entry {
        uint64_t handle = 0;
        ObjectID tree_id;
        AnimationMixer *tree = nullptr;
        uint64_t parent = 0;
        bool dirty = false;
        int kind = 0;
        LocalVector<Entry *> children;
        LocalVector<Ref<SkeletonAnimationPose>> poses;
        AnimationMixer::AnimationCallbackModeProcess old_mode;
        double delta = 0;
        bool sample_pose = true, display_pose = true, exact_pose = true;
        double display_time = 0, interval = 0;
        bool queued = false;
        bool prepared = false;
        bool published = false;
        bool removed = false;
        String fallback;
    };
    struct Batch {
        uint32_t start = 0, end = 0;
        std::atomic<bool> completed{false};
        Semaphore ready;
    };
    LocalVector<Entry *> entries;
    HashMap<uint64_t, Entry *> handles;
    LocalVector<Entry *> work;
    struct Unit { uint32_t start = 0, end = 0; };
    LocalVector<Unit> units;
    LocalVector<Batch *> batches;
    uint64_t next_handle = 1;
    uint64_t frame_id = 0;
    WorkerThreadPool::GroupID group = -1;
    int batch_count = 0, worker_count = 0, fallback_count = 0, early_wait_count = 0, failed_count = 0;
    bool pending = false, publishing = false;
    int recomputed_count = 0;
    bool preparing = false;
    int mutation_depth = 0;
    // Per-frame publish observability; reset in submit(). Main thread only.
    struct MergeRunTracker {
        uint32_t quiet_entries = 0; // Entries whose whole publish had no observable operation.
        uint32_t mergeable_skins = 0; // Skin uploads inside runs of at least two quiet entries.
        uint32_t mergeable_runs = 0;
        uint32_t max_run = 0;
        uint32_t current_run = 0, run_head_skins = 0;
        void push(bool p_quiet, uint32_t p_skins);
    };
    uint32_t publish_entry_count = 0, publish_fallback_publishes = 0;
    uint32_t publish_skin_uploads = 0;
    uint64_t publish_skin_upload_bytes = 0;
    uint32_t publish_method_events = 0, publish_audio_events = 0;
    uint32_t publish_resource_signals = 0, publish_deferred_signals = 0;
    uint32_t publish_mixer_applied_observers = 0, publish_applied_tracks = 0;
    uint32_t publish_pose_updated_observers = 0, publish_skeleton_updated_observers = 0;
    uint32_t publish_attachments = 0, publish_fast_path_entries = 0;
    uint32_t publish_target_writes = 0, publish_modifier_signals = 0;
    MergeRunTracker publish_strict_merge, publish_fast_merge;
    void _reset_publish_stats();
    void _record_publish_stats(Entry *p_entry);
    int64_t _register(AnimationMixer *p_mixer, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, int64_t p_parent, int p_kind);
    void _prepare_entry(Entry *p_entry);
    void _evaluate_entry(Entry *p_entry);
    void _remove_entry(Entry *p_entry);
    void _execute_batch(uint32_t p_index, bool p_unused);
    void _wait_batch(int p_index);
    void _publish_entry(Entry *p_entry);
    static LocalVector<AnimationBatchProcessor *> active_processors;

protected:
    static void _bind_methods();

public:
    int64_t register_tree(AnimationTree *p_tree, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods);
    int64_t register_player(AnimationPlayer *p_player, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, int64_t p_parent = 0);
    void unregister_animation(int64_t p_handle);
    // Main-thread join only; the frame remains pending and unpublished.
    void complete();
    // Main-thread publication for one registered entry; repeated calls are no-ops.
    void publish(int64_t p_handle);
    void discard(int64_t p_handle);
    void unregister_tree(int64_t p_handle);
    bool queue_update(int64_t p_handle, double p_delta);
    bool queue_update_with_options(int64_t p_handle, double p_delta, bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval);
    int64_t submit(int p_batch_size = 8, int p_max_workers = 8);
    void complete_batch(int64_t p_handle);
    // Main-thread join and publication of every remaining entry in registration order.
    void complete_and_publish();
    void finish_all() { complete_and_publish(); }
    // A live mixer mutation finishes its dependency: AnimationTree publishes first;
    // AnimationPlayer is marked dirty for main-thread recomputation.
    void finish_for_dependency(AnimationMixer *p_mixer = nullptr);
    bool is_pending() const { return pending; }
    bool was_evaluated(int64_t p_handle) const;
    Dictionary get_statistics() const;
    String get_fallback_reason(int64_t p_handle) const;
    // Global scene/resource/target mutation guard; shared dependencies require this
    // protection instead of narrowing completion to one apparently affected entry.
    static void finish_pending_frames();
    ~AnimationBatchProcessor();
};

// Player mutations operate on live state after workers have joined. Skeleton setters
// invoked by Seek/Stop must not publish unrelated pending animation snapshots.
class AnimationBatchMutationScope {
    Ref<AnimationBatchProcessor> owner;
public:
    explicit AnimationBatchMutationScope(AnimationBatchProcessor *p_owner) {
        if (p_owner && p_owner->pending) { owner = Ref<AnimationBatchProcessor>(p_owner); owner->mutation_depth++; }
    }
    ~AnimationBatchMutationScope() { if (owner.is_valid()) { owner->mutation_depth--; } }
};
#endif
