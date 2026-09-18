#include "animation_batch_processor.h"

#ifndef _3D_DISABLED
#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/profiling/profiling.h"

LocalVector<AnimationBatchProcessor *> AnimationBatchProcessor::active_processors;

void AnimationBatchProcessor::_bind_methods() {
    ClassDB::bind_method(D_METHOD("register_tree", "tree", "poses", "safe_methods"), &AnimationBatchProcessor::register_tree);
    ClassDB::bind_method(D_METHOD("register_player", "player", "poses", "safe_methods", "parent"), &AnimationBatchProcessor::register_player, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("unregister_tree", "handle"), &AnimationBatchProcessor::unregister_tree);
    ClassDB::bind_method(D_METHOD("unregister_animation", "handle"), &AnimationBatchProcessor::unregister_animation);
    ClassDB::bind_method(D_METHOD("queue_update", "handle", "delta"), &AnimationBatchProcessor::queue_update);
    ClassDB::bind_method(D_METHOD("queue_update_with_options", "handle", "delta", "sample_pose", "display_pose", "exact_pose", "display_time", "interval"), &AnimationBatchProcessor::queue_update_with_options);
    ClassDB::bind_method(D_METHOD("submit", "batch_size", "max_workers"), &AnimationBatchProcessor::submit, DEFVAL(8), DEFVAL(8));
    ClassDB::bind_method(D_METHOD("complete_batch", "handle"), &AnimationBatchProcessor::complete_batch);
    ClassDB::bind_method(D_METHOD("complete"), &AnimationBatchProcessor::complete);
    ClassDB::bind_method(D_METHOD("publish", "handle"), &AnimationBatchProcessor::publish);
    ClassDB::bind_method(D_METHOD("discard", "handle"), &AnimationBatchProcessor::discard);
    ClassDB::bind_method(D_METHOD("complete_and_publish"), &AnimationBatchProcessor::complete_and_publish);
    ClassDB::bind_method(D_METHOD("finish_all"), &AnimationBatchProcessor::finish_all);
    ClassDB::bind_method(D_METHOD("is_pending"), &AnimationBatchProcessor::is_pending);
    ClassDB::bind_method(D_METHOD("was_evaluated", "handle"), &AnimationBatchProcessor::was_evaluated);
    ClassDB::bind_method(D_METHOD("get_statistics"), &AnimationBatchProcessor::get_statistics);
    ClassDB::bind_method(D_METHOD("get_fallback_reason", "handle"), &AnimationBatchProcessor::get_fallback_reason);
}

int64_t AnimationBatchProcessor::register_tree(AnimationTree *p_tree, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods) {
    return _register(p_tree, p_poses, p_safe_methods, 0, 0);
}

int64_t AnimationBatchProcessor::register_player(AnimationPlayer *p_player, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, int64_t p_parent) {
    return _register(p_player, p_poses, p_safe_methods, p_parent, p_parent ? 2 : 1);
}

int64_t AnimationBatchProcessor::_register(AnimationMixer *p_tree, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, int64_t p_parent, int p_kind) {
    ERR_FAIL_COND_V(!Thread::is_main_thread() || publishing, 0);
    complete_and_publish();
    ERR_FAIL_COND_V(pending, 0);
    ERR_FAIL_NULL_V(p_tree, 0);
    for (Entry *entry : entries) { if (entry->tree == p_tree) { return entry->handle; } }
    ERR_FAIL_COND_V(p_tree->batch_owner != nullptr, 0);
    ERR_FAIL_COND_V(p_parent && (!handles.has(p_parent) || handles[p_parent]->parent), 0);
    Entry *entry = memnew(Entry);
    entry->handle = next_handle++;
    entry->tree = p_tree;
    entry->tree_id = p_tree->get_instance_id();
    entry->parent = p_parent;
    entry->kind = p_kind;
    entry->old_mode = p_tree->get_callback_mode_process();
    for (int i = 0; i < p_poses.size(); ++i) {
        Ref<SkeletonAnimationPose> pose = p_poses[i];
        if (pose.is_null()) { continue; }
        entry->poses.push_back(pose);
        AnimationMixer::BatchPoseBinding binding;
        binding.skeleton_id = pose->get_skeleton_id();
        binding.pose = pose;
        p_tree->batch_poses.push_back(binding);
    }
    for (const String &method : p_safe_methods) { p_tree->batch_safe_methods.insert(StringName(method)); }
    const bool was_processing = p_tree->processing;
    p_tree->_set_process(false);
    p_tree->callback_mode_process = AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL;
    p_tree->_set_process(was_processing);
    p_tree->batch_owner = this;
    p_tree->batch_bindings_checked = false;
    entries.push_back(entry);
    handles.insert(entry->handle, entry);
    if (p_parent) { handles[p_parent]->children.push_back(entry); }
    return entry->handle;
}

void AnimationBatchProcessor::unregister_tree(int64_t p_handle) { unregister_animation(p_handle); }

void AnimationBatchProcessor::unregister_animation(int64_t p_handle) {
    ERR_FAIL_COND(!Thread::is_main_thread());
    complete();
    Entry **found = handles.getptr(p_handle);
    if (!found) { return; }
    // Do not emit other objects' completion events from an exit-tree callback.
    for (Entry *entry : entries) {
        if (entry->handle == uint64_t(p_handle) || entry->parent == uint64_t(p_handle)) { entry->removed = true; }
    }
    if (pending || publishing || preparing) { return; }
    for (int i = int(entries.size()) - 1; i >= 0; --i) {
        if (entries[i]->removed) { _remove_entry(entries[i]); }
    }
}

void AnimationBatchProcessor::_remove_entry(Entry *entry) {
    if (entry->parent && handles.has(entry->parent)) { handles[entry->parent]->children.erase(entry); }
    if (ObjectDB::get_instance(entry->tree_id) == entry->tree) {
        entry->tree->_discard_batch();
        entry->tree->batch_owner = nullptr;
        entry->tree->batch_poses.clear();
        entry->tree->batch_safe_methods.clear();
        const bool was_processing = entry->tree->processing;
        entry->tree->_set_process(false);
        entry->tree->callback_mode_process = entry->old_mode;
        entry->tree->_set_process(was_processing);
    }
    for (const Ref<SkeletonAnimationPose> &pose : entry->poses) { pose->release(); }
    work.erase(entry);
    handles.erase(entry->handle);
    entries.erase(entry);
    memdelete(entry);
}

bool AnimationBatchProcessor::queue_update(int64_t p_handle, double p_delta) {
    return queue_update_with_options(p_handle, p_delta, true, true, true, 0, 0);
}

bool AnimationBatchProcessor::queue_update_with_options(int64_t p_handle, double p_delta, bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval) {
    ERR_FAIL_COND_V(!Thread::is_main_thread() || pending || publishing, false);
    Entry **found = handles.getptr(p_handle);
    ERR_FAIL_NULL_V(found, false);
    Entry *entry = *found;
    ERR_FAIL_COND_V(entry->queued || entry->removed, false);
    ERR_FAIL_COND_V(!Math::is_finite(p_time) || !Math::is_finite(p_interval) || p_interval < 0, false);
    entry->delta = p_delta;
    entry->sample_pose = p_sample;
    entry->display_pose = p_display;
    entry->exact_pose = p_exact;
    entry->display_time = p_time;
    entry->interval = p_interval;
    entry->queued = true;
    return true;
}

void AnimationBatchProcessor::_prepare_entry(Entry *entry) {
    entry->prepared = true;
    entry->dirty = false;
    entry->fallback = String();
    entry->tree->batch_sample_pose = entry->sample_pose;
    entry->tree->batch_display_pose = entry->display_pose;
    for (const Ref<SkeletonAnimationPose> &pose : entry->poses) {
        if (!pose->prepare_frame(entry->sample_pose, entry->display_pose, entry->exact_pose, entry->display_time, entry->interval)) {
            entry->prepared = false;
            entry->fallback = pose->get_fallback_reason();
            break;
        }
    }
    if (entry->prepared) {
        for (const Ref<SkeletonAnimationPose> &pose : entry->poses) {
            entry->tree->batch_sample_pose |= pose->needs_sample();
            entry->tree->batch_display_pose |= pose->needs_display();
        }
        if (entry->tree->batch_sample_pose && !entry->sample_pose) {
            for (const Ref<SkeletonAnimationPose> &pose : entry->poses) {
                if (!pose->needs_sample() && !pose->prepare_frame(true, entry->tree->batch_display_pose, true, entry->display_time, entry->interval)) { entry->prepared = false; }
            }
        }
        entry->prepared = entry->prepared && entry->tree->_prepare_batch(entry->delta);
        if (!entry->prepared) { entry->fallback = entry->tree->batch_fallback_reason; }
    }
}

int64_t AnimationBatchProcessor::submit(int p_batch_size, int p_max_workers) {
    ERR_FAIL_COND_V(!Thread::is_main_thread() || pending || publishing, 0);
    ERR_FAIL_COND_V(p_batch_size < 1 || p_max_workers < 1, 0);
    GodotProfileZone("Animation.Prepare");
    work.clear();
    units.clear();
    fallback_count = early_wait_count = failed_count = recomputed_count = 0;
    _reset_publish_stats();
    preparing = true;
    for (Entry *entry : entries) {
        if (entry->removed || ObjectDB::get_instance(entry->tree_id) != entry->tree) {
            entry->queued = false;
            entry->prepared = false;
            continue;
        }
        if (!entry->queued) { continue; }
        entry->published = false;
        _prepare_entry(entry);
        if (!entry->prepared) { fallback_count++; }
    }
    preparing = false;
    // A body and its weapon stay on the same worker; animals form independent units.
    for (Entry *entry : entries) {
        if (!entry->queued || entry->removed) { continue; }
        if (entry->parent && handles.has(entry->parent) && handles[entry->parent]->queued) { continue; }
        Unit unit;
        unit.start = work.size();
        work.push_back(entry);
        for (Entry *child : entry->children) {
            if (child->queued && !child->removed) { work.push_back(child); }
        }
        unit.end = work.size();
        units.push_back(unit);
    }
    for (Entry *entry : entries) { entry->queued = false; }
    batch_count = (units.size() + p_batch_size - 1) / p_batch_size;
    while (batches.size() < uint32_t(batch_count)) { batches.push_back(memnew(Batch)); }
    for (int i = 0; i < batch_count; ++i) {
        Batch *batch = batches[i];
        while (batch->ready.try_wait()) {}
        batch->completed.store(false, std::memory_order_relaxed);
        batch->start = units[i * p_batch_size].start;
        batch->end = units[MIN(uint32_t((i + 1) * p_batch_size), units.size()) - 1].end;
    }
    frame_id++;
    pending = !work.is_empty();
    if (!pending) { worker_count = 0; return frame_id; }
    active_processors.push_back(this);
    worker_count = MIN(MIN(batch_count, p_max_workers), MIN(WorkerThreadPool::get_singleton()->get_thread_count(), MAX(1, OS::get_singleton()->get_processor_count() - 2)));
    if (units.size() < 16 || worker_count <= 1) {
        worker_count = 1;
        for (int i = 0; i < batch_count; ++i) { _execute_batch(i, false); }
    } else {
        GodotProfileZone("Animation.Launch");
        group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &AnimationBatchProcessor::_execute_batch, false, batch_count, worker_count, true, "Animation batches");
    }
    return frame_id;
}

void AnimationBatchProcessor::_evaluate_entry(Entry *entry) {
    if (!entry->prepared) { return; }
    if (entry->parent && handles.has(entry->parent) && !handles[entry->parent]->prepared) {
        entry->dirty = true;
        return;
    }
    if (entry->kind == 2) {
        GodotProfileZone("Animation.Weapon");
        entry->tree->_evaluate_batch();
        for (const Ref<SkeletonAnimationPose> &pose : entry->poses) { pose->evaluate_sockets(); }
    } else if (entry->kind == 1) {
        GodotProfileZone("Animation.Animal");
        entry->tree->_evaluate_batch();
    } else { entry->tree->_evaluate_batch(); }
}

void AnimationBatchProcessor::_execute_batch(uint32_t p_index, bool p_unused) {
    GodotProfileZone("Animation.Batch");
    Batch *batch = batches[p_index];
    for (uint32_t i = batch->start; i < batch->end; ++i) { _evaluate_entry(work[i]); }
    batch->completed.store(true, std::memory_order_release);
    batch->ready.post();
}

void AnimationBatchProcessor::_wait_batch(int p_index) {
    if (!batches[p_index]->completed.load(std::memory_order_acquire)) { batches[p_index]->ready.wait(); }
}

void AnimationBatchProcessor::complete() {
    ERR_FAIL_COND(!Thread::is_main_thread());
    if (group != -1) {
        GodotProfileZone("Animation.Wait");
        WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
        group = -1;
    }
}

void AnimationBatchProcessor::_publish_entry(Entry *entry) {
    if (entry->published || entry->removed || ObjectDB::get_instance(entry->tree_id) != entry->tree) { return; }
    if (entry->dirty) {
        GodotProfileZone("Animation.Recompute");
        entry->tree->_discard_batch();
        preparing = true;
        _prepare_entry(entry);
        preparing = false;
        // The parent fallback has now published its actual attachment. Capture it on main.
        if (entry->prepared) {
            if (entry->parent && handles.has(entry->parent) && !handles[entry->parent]->prepared) {
                for (const Ref<SkeletonAnimationPose> &pose : entry->poses) {
                    pose->configure_attachment(Ref<SkeletonAnimationPose>(), -1, Transform3D(), Transform3D(), false);
                }
            }
            entry->tree->_evaluate_batch();
            for (const Ref<SkeletonAnimationPose> &pose : entry->poses) { pose->evaluate_sockets(); }
            recomputed_count++;
        }
    }
    entry->published = true;
    if (entry->prepared) {
        entry->tree->_publish_batch();
        if (ObjectDB::get_instance(entry->tree_id) == entry->tree && !entry->tree->batch_succeeded) { failed_count++; }
    } else {
        GodotProfileZone("Animation.Fallback");
        for (const Ref<SkeletonAnimationPose> &pose : entry->poses) { pose->release(); }
        entry->tree->advance(entry->delta);
    }
    _record_publish_stats(entry);
}

void AnimationBatchProcessor::MergeRunTracker::push(bool p_quiet, uint32_t p_skins) {
    if (!p_quiet) {
        current_run = 0;
        run_head_skins = 0;
        return;
    }
    quiet_entries++;
    current_run++;
    if (current_run == 1) {
        run_head_skins = p_skins;
    } else if (current_run == 2) {
        mergeable_runs++;
        mergeable_skins += run_head_skins + p_skins;
    } else {
        mergeable_skins += p_skins;
    }
    max_run = MAX(max_run, current_run);
}

void AnimationBatchProcessor::_reset_publish_stats() {
    publish_entry_count = publish_fallback_publishes = 0;
    publish_skin_uploads = publish_skin_upload_bytes = 0;
    publish_method_events = publish_audio_events = 0;
    publish_resource_signals = publish_deferred_signals = 0;
    publish_mixer_applied_observers = publish_applied_tracks = 0;
    publish_pose_updated_observers = publish_skeleton_updated_observers = 0;
    publish_attachments = publish_fast_path_entries = 0;
    publish_target_writes = publish_modifier_signals = 0;
    publish_strict_merge = MergeRunTracker();
    publish_fast_merge = MergeRunTracker();
}

void AnimationBatchProcessor::_record_publish_stats(Entry *entry) {
    publish_entry_count++;
    if (!entry->prepared) {
        // The legacy advance path runs its own events and signals; treat it as observable.
        publish_fallback_publishes++;
        publish_strict_merge.push(false, 0);
        publish_fast_merge.push(false, 0);
        return;
    }
    const bool mixer_valid = ObjectDB::get_instance(entry->tree_id) == entry->tree;
    bool mixer_quiet = true;
    if (mixer_valid) {
        AnimationMixer *tree = entry->tree;
        publish_method_events += tree->publish_method_events;
        publish_audio_events += tree->publish_audio_events;
        publish_resource_signals += tree->publish_resource_signals;
        publish_deferred_signals += tree->publish_deferred_signals;
        publish_applied_tracks += tree->publish_applied_tracks;
        if (tree->publish_mixer_applied_observers) { publish_mixer_applied_observers++; }
        mixer_quiet = tree->publish_method_events == 0 && tree->publish_audio_events == 0 &&
                tree->publish_resource_signals == 0 && tree->publish_deferred_signals == 0 &&
                tree->publish_applied_tracks == 0 && !tree->publish_mixer_applied_observers;
    }
    uint32_t entry_skins = 0;
    bool strict_poses = true, fast_poses = true;
    for (const Ref<SkeletonAnimationPose> &pose : entry->poses) {
        const SkeletonAnimationPose::PublishStats &stats = pose->get_publish_stats();
        entry_skins += stats.skins;
        publish_skin_uploads += stats.skins;
        publish_skin_upload_bytes += stats.skin_bytes;
        publish_attachments += stats.attachments;
        publish_target_writes += stats.target_writes;
        publish_modifier_signals += stats.modifier_signals;
        if (stats.pose_updated_observers) { publish_pose_updated_observers++; }
        if (stats.skeleton_updated_observers) { publish_skeleton_updated_observers++; }
        if (stats.fast_path) { publish_fast_path_entries++; }
        strict_poses = strict_poses && stats.pose_updated_observers == 0 && stats.skeleton_updated_observers == 0 &&
                stats.target_writes == 0 && stats.modifier_signals == 0;
        // The fast path only exists when every observer is a plain native attachment.
        fast_poses = fast_poses && stats.pose_updated_observers == 0 && stats.fast_path &&
                stats.target_writes == 0 && stats.modifier_signals == 0;
    }
    if (!mixer_valid) { strict_poses = fast_poses = false; }
    publish_strict_merge.push(mixer_quiet && strict_poses, entry_skins);
    publish_fast_merge.push(mixer_quiet && fast_poses, entry_skins);
}

void AnimationBatchProcessor::publish(int64_t p_handle) {
    ERR_FAIL_COND(!Thread::is_main_thread());
    if (!pending || publishing) { return; }
    complete();
    Entry **entry = handles.getptr(p_handle);
    if (!entry || !work.has(*entry)) { return; }
    publishing = true;
    _publish_entry(*entry);
    publishing = false;
}

void AnimationBatchProcessor::discard(int64_t p_handle) {
    ERR_FAIL_COND(!Thread::is_main_thread());
    complete();
    Entry **entry = handles.getptr(p_handle);
    if (!pending || !entry || (*entry)->published || (*entry)->removed || !work.has(*entry)) { return; }
    (*entry)->published = true;
    if (ObjectDB::get_instance((*entry)->tree_id) == (*entry)->tree) {
        (*entry)->tree->_discard_batch();
    }
}

void AnimationBatchProcessor::complete_batch(int64_t p_handle) {
    ERR_FAIL_COND(!Thread::is_main_thread());
    if (!pending || publishing) { return; }
    for (int b = 0; b < batch_count; ++b) {
        for (uint32_t i = batches[b]->start; i < batches[b]->end; ++i) {
            if (work[i]->handle == uint64_t(p_handle)) {
                early_wait_count++;
                GodotProfileZone("Animation.Wait");
                _wait_batch(b);
                return;
            }
        }
    }
}

void AnimationBatchProcessor::complete_and_publish() {
    ERR_FAIL_COND(!Thread::is_main_thread());
    if (!pending || publishing || preparing || mutation_depth) { return; }
    complete();
    publishing = true;
    { GodotProfileZone("Animation.Publish"); for (Entry *entry : work) { _publish_entry(entry); } }
    publishing = false;
    pending = false;
    active_processors.erase(this);
    for (int i = int(entries.size()) - 1; i >= 0; --i) {
        if (entries[i]->removed) { _remove_entry(entries[i]); }
    }
}

void AnimationBatchProcessor::finish_for_dependency(AnimationMixer *p_mixer) {
    if (!pending || preparing) { return; }
    for (Entry *entry : work) {
        if (entry->tree != p_mixer || entry->published || entry->removed) { continue; }
        early_wait_count++;
        complete();
        if (Object::cast_to<AnimationPlayer>(p_mixer)) { entry->dirty = true; }
        else if (!publishing) { publish(entry->handle); }
        return;
    }
}

void AnimationBatchProcessor::finish_pending_frames() {
    // Publication callbacks may change bones; never recursively publish that frame.
    for (int i = int(active_processors.size()) - 1; i >= 0; --i) {
        AnimationBatchProcessor *processor = active_processors[i];
        if (!processor->publishing && !processor->preparing && !processor->mutation_depth) { processor->complete_and_publish(); }
    }
}

Dictionary AnimationBatchProcessor::get_statistics() const {
    Dictionary result;
    result["frame"] = frame_id;
    result["work_units"] = units.size();
    result["batches"] = batch_count;
    result["workers"] = worker_count;
    result["fallbacks"] = fallback_count;
    result["early_waits"] = early_wait_count;
    result["failed"] = failed_count;
    result["recomputed"] = recomputed_count;
    int counts[3] = {}, fallbacks[3] = {}, evaluated[3] = {};
    int samples = 0, displays = 0, exact = 0;
    uint64_t copies = 0;
    for (const Entry *entry : work) {
        if (entry->removed || ObjectDB::get_instance(entry->tree_id) != entry->tree) { continue; }
        counts[entry->kind]++;
        fallbacks[entry->kind] += !entry->prepared;
        if (group == -1) {
            evaluated[entry->kind] += entry->prepared && entry->published && entry->tree->batch_succeeded;
            samples += entry->tree->batch_sample_pose;
            displays += entry->tree->batch_display_pose;
            exact += entry->exact_pose;
            for (const Ref<SkeletonAnimationPose> &pose : entry->poses) { copies += pose->get_skin_buffer_copies(); }
        }
    }
    const char *names[] = { "pawns", "animals", "weapons" };
    for (int i = 0; i < 3; i++) {
        result[names[i]] = counts[i];
        result[String(names[i]) + "_fallbacks"] = fallbacks[i];
        result[String(names[i]) + "_evaluated"] = evaluated[i];
    }
    result["samples"] = samples;
    result["displays"] = displays;
    result["exact"] = exact;
    result["skin_buffer_copies"] = copies;
    // Observability of the current frame's publish round (reset by submit()).
    result["publish_entries"] = publish_entry_count;
    result["publish_fallback_publishes"] = publish_fallback_publishes;
    result["publish_skin_uploads"] = publish_skin_uploads;
    result["publish_skin_upload_bytes"] = publish_skin_upload_bytes;
    result["publish_method_events"] = publish_method_events;
    result["publish_audio_events"] = publish_audio_events;
    result["publish_resource_signals"] = publish_resource_signals;
    result["publish_deferred_signals"] = publish_deferred_signals;
    result["publish_mixer_applied_observers"] = publish_mixer_applied_observers;
    result["publish_applied_tracks"] = publish_applied_tracks;
    result["publish_pose_updated_observers"] = publish_pose_updated_observers;
    result["publish_skeleton_updated_observers"] = publish_skeleton_updated_observers;
    result["publish_attachments"] = publish_attachments;
    result["publish_fast_path_entries"] = publish_fast_path_entries;
    result["publish_target_writes"] = publish_target_writes;
    result["publish_modifier_signals"] = publish_modifier_signals;
    // Strict: any observable operation (events, signals, observers, applied tracks,
    // attachments) breaks mergeability. Fast: additionally allows the native-only
    // attachment fast path, which a batched submit would have to prove safe.
    result["publish_strict_quiet"] = publish_strict_merge.quiet_entries;
    result["publish_strict_merge_skins"] = publish_strict_merge.mergeable_skins;
    result["publish_strict_merge_runs"] = publish_strict_merge.mergeable_runs;
    result["publish_strict_max_run"] = publish_strict_merge.max_run;
    result["publish_fast_quiet"] = publish_fast_merge.quiet_entries;
    result["publish_fast_merge_skins"] = publish_fast_merge.mergeable_skins;
    result["publish_fast_merge_runs"] = publish_fast_merge.mergeable_runs;
    result["publish_fast_max_run"] = publish_fast_merge.max_run;
    return result;
}

String AnimationBatchProcessor::get_fallback_reason(int64_t p_handle) const {
    Entry *const *entry = handles.getptr(p_handle);
    return entry ? (*entry)->fallback : String("Invalid registration");
}

bool AnimationBatchProcessor::was_evaluated(int64_t p_handle) const {
    Entry *const *entry = handles.getptr(p_handle);
    if (!(group == -1 && entry && !(*entry)->removed && (*entry)->published && (*entry)->prepared && ObjectDB::get_instance((*entry)->tree_id) == (*entry)->tree && (*entry)->tree->batch_succeeded)) { return false; }
    for (const Ref<SkeletonAnimationPose> &pose : (*entry)->poses) {
        if (!pose->matches_current_inputs()) { return false; }
    }
    return true;
}

AnimationBatchProcessor::~AnimationBatchProcessor() {
    finish_all();
    while (!entries.is_empty()) { unregister_animation(entries[0]->handle); }
    for (Batch *batch : batches) { memdelete(batch); }
}

#endif
