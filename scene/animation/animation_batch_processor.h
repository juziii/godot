#pragma once

#ifndef _3D_DISABLED
#include "core/object/ref_counted.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/semaphore.h"
#include "core/templates/hash_map.h"
#include "scene/animation/animation_tree.h"
#include "scene/3d/skeleton_animation_pose.h"
#include <atomic>

// A frame owns its inputs until complete_and_publish(). No worker accesses the scene tree.
class AnimationBatchProcessor : public RefCounted {
    GDCLASS(AnimationBatchProcessor, RefCounted);
    struct Entry {
        uint64_t handle = 0;
        ObjectID tree_id;
        AnimationTree *tree = nullptr;
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
    LocalVector<Batch *> batches;
    uint64_t next_handle = 1;
    uint64_t frame_id = 0;
    WorkerThreadPool::GroupID group = -1;
    int batch_count = 0, worker_count = 0, fallback_count = 0, early_wait_count = 0, failed_count = 0;
    bool pending = false, publishing = false;
    void _remove_entry(Entry *p_entry);
    void _execute_batch(uint32_t p_index, bool p_unused);
    void _wait_batch(int p_index);
    void _publish_entry(Entry *p_entry);
    static LocalVector<AnimationBatchProcessor *> active_processors;

protected:
    static void _bind_methods();

public:
    int64_t register_tree(AnimationTree *p_tree, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods);
    void unregister_tree(int64_t p_handle);
    bool queue_update(int64_t p_handle, double p_delta);
    bool queue_update_with_options(int64_t p_handle, double p_delta, bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval);
    int64_t submit(int p_batch_size = 8, int p_max_workers = 8);
    void complete_batch(int64_t p_handle);
    void complete_and_publish();
    void finish_all() { complete_and_publish(); }
    void finish_for_dependency() { if (pending) { early_wait_count++; complete_and_publish(); } }
    bool is_pending() const { return pending; }
    bool was_evaluated(int64_t p_handle) const;
    Dictionary get_statistics() const;
    String get_fallback_reason(int64_t p_handle) const;
    static void finish_pending_frames();
    ~AnimationBatchProcessor();
};
#endif
