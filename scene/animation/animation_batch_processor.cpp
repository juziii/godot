#include "animation_batch_processor.h"

#ifndef _3D_DISABLED
#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/profiling/profiling.h"
#include "scene/animation/animation_node_state_machine.h"

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
    if (entry) { (*entry)->published = true; }
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

void AnimationMixer::finish_pending_animation() {
    if (batch_owner && Thread::is_main_thread()) { batch_owner->finish_for_dependency(this); }
}

SkeletonAnimationPose *AnimationMixer::_get_batch_pose(ObjectID p_id) const {
    for (const BatchPoseBinding &binding : batch_poses) {
        if (binding.skeleton_id == p_id) { return binding.pose.ptr(); }
    }
    return nullptr;
}

bool AnimationMixer::_prepare_batch_graph() { return false; }

bool AnimationMixer::_prepare_batch(double p_delta) {
    batch_fallback_reason = String();
    batch_delta = p_delta;
    batch_initial_instances.clear();
    batch_instance_offset = 0;
    batch_succeeded = false;
    batch_signals.clear();
    batch_method_events.clear();
    batch_audio_events.clear();
    batch_resource_signals.clear();
    if (get_script_instance() || capture_cache.animation.is_valid()) {
        batch_fallback_reason = "Script mixer or capture animation";
        return false;
    }
    if (!cache_valid && !_update_caches()) {
        batch_fallback_reason = "Unresolved animation bindings";
        return false;
    }
    if (!_prepare_batch_graph()) { return false; }
    if (batch_bindings_checked) { batch_fallback_reason = batch_binding_reason; return batch_binding_reason.is_empty(); }
    batch_bindings_checked = true;
    batch_event_tracks.clear();
    for (const KeyValue<StringName, AnimationData> &kv : animation_set) {
        const Ref<Animation> &animation = kv.value.animation;
        LocalVector<int> &indices = batch_event_tracks[animation];
        for (int i = 0; i < animation->get_track_count(); i++) {
            const Animation::TrackType type = animation->track_get_type(i);
            if (type == Animation::TYPE_METHOD || type == Animation::TYPE_AUDIO) { indices.push_back(i); }
        }
    }
    for (const KeyValue<Animation::TrackCacheID, TrackCache *> &kv : track_cache) {
        TrackCache *track = kv.value;
        if (track->type == Animation::TYPE_POSITION_3D) {
            TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
            if (!track->root_motion && (t->bone_idx < 0 || !_get_batch_pose(t->skeleton_id))) {
                batch_fallback_reason = "Transform outside registered skeletons";
                batch_binding_reason = batch_fallback_reason;
                return false;
            }
        } else if (track->type != Animation::TYPE_METHOD && track->type != Animation::TYPE_AUDIO && track->type != Animation::TYPE_BLEND_SHAPE) {
            batch_fallback_reason = "Unsupported property or nested animation track";
            batch_binding_reason = batch_fallback_reason;
            return false;
        }
    }
    for (const KeyValue<StringName, AnimationData> &kv : animation_set) {
        const Ref<Animation> &a = kv.value.animation;
        for (int track = 0; track < a->get_track_count(); ++track) {
            if (!a->track_is_enabled(track) || a->track_get_type(track) != Animation::TYPE_METHOD) { continue; }
            for (int key = 0; key < a->track_get_key_count(track); ++key) {
                if (!batch_safe_methods.has(a->method_track_get_name(track, key))) {
                    batch_fallback_reason = "Immediate method track requires synchronous evaluation";
                    batch_binding_reason = batch_fallback_reason;
                    return false;
                }
            }
        }
    }
    batch_binding_reason = String();
    return true;
}

void AnimationMixer::_evaluate_batch() {
    batch_evaluating = true;
    _evaluate_batch_start();
    { GodotProfileZone("Animation.Init"); _blend_init(); }
    if (cache_valid && _blend_pre_process(batch_delta, track_count, track_map)) {
        { GodotProfileZone("Animation.Weights"); _blend_calc_total_weight(); }
        { GodotProfileZone("Animation.SampleBlend"); _blend_process(batch_delta, false); }
        _blend_apply();
        { GodotProfileZone("Animation.Skeleton");
            for (BatchPoseBinding &binding : batch_poses) {
                if (batch_sample_pose) { binding.pose->evaluate(); }
                else { binding.pose->interpolate_frame(); }
            }
        }
        batch_succeeded = true;
    }
    if (!batch_succeeded && Object::cast_to<AnimationPlayer>(this)) {
        for (BatchPoseBinding &binding : batch_poses) { binding.pose->evaluate(); }
        batch_succeeded = true;
    }
    batch_evaluating = false;
}

void AnimationMixer::_publish_batch() {
    const ObjectID mixer_id = get_instance_id();
    // Observability snapshot for Animation.Publish profiling; reset for this round.
    publish_method_events = batch_method_events.size();
    publish_audio_events = batch_audio_events.size();
    publish_resource_signals = batch_resource_signals.size();
    publish_deferred_signals = batch_signals.size();
    publish_mixer_applied_observers = has_connections(SNAME("mixer_applied"));
    publish_applied_tracks = 0;
    for (const BatchPoseBinding &binding : batch_poses) { binding.pose->reset_publish_stats(); }
    if (batch_succeeded) {
        _commit_batch_state();
        { GodotProfileZone("Animation.Events");
            batch_event_pass = true;
            if (batch_audio_events.is_empty()) {
                for (uint32_t i = 0; i < batch_method_events.size(); i++) {
                    const BatchMethodEvent event = batch_method_events[i];
                    _call_object(event.target, event.method, event.arguments, event.deferred);
                    if (ObjectDB::get_instance(mixer_id) != this) { return; }
                    if (!cache_valid) { batch_succeeded = false; break; }
                }
            } else {
                if (!batch_initial_instances.is_empty()) {
                    for (const AnimationInstance &instance : animation_instances) { batch_initial_instances.push_back(instance); }
                    SWAP(animation_instances, batch_initial_instances);
                }
                _blend_process(batch_delta, false);
            }
            if (ObjectDB::get_instance(mixer_id) != this) { return; }
            batch_event_pass = false;
            if (!batch_succeeded || !cache_valid) {
                batch_succeeded = false;
                clear_animation_instances();
                batch_method_events.clear();
                batch_audio_events.clear();
                batch_resource_signals.clear();
                batch_signals.clear();
                return;
            }
        }
        { GodotProfileZone("Animation.PublishPoses");
            for (uint32_t i = 0; batch_display_pose && i < batch_poses.size(); i++) {
                Ref<SkeletonAnimationPose> pose = batch_poses[i].pose;
                const bool published = pose->publish();
                if (ObjectDB::get_instance(mixer_id) != this) { return; }
                if (!published) { batch_succeeded = false; break; }
            }
        }
        { GodotProfileZone("Animation.PublishApply");
            batch_publishing = true;
            _blend_apply();
            batch_publishing = false;
        }
        { GodotProfileZone("Animation.PublishPostProcess"); _blend_post_process(); }
        if (ObjectDB::get_instance(mixer_id) != this) { return; }
        { GodotProfileZone("Animation.PublishSignals");
            emit_signal(SNAME("mixer_applied"));
            for (const BatchResourceSignal &event : batch_resource_signals) { event.target->emit_signal(event.signal, event.value); }
            for (const Pair<StringName, StringName> &event : batch_signals) { call_deferred(SNAME("emit_signal"), event.first, event.second); }
        }
    }
    { GodotProfileZone("Animation.PublishCleanup");
        clear_animation_instances();
        batch_signals.clear();
        batch_method_events.clear();
        batch_audio_events.clear();
        batch_resource_signals.clear();
    }
}

void AnimationMixer::queue_resource_signal(const Ref<Resource> &p_target, const StringName &p_signal, const StringName &p_value) {
    BatchResourceSignal event;
    event.target = p_target;
    event.signal = p_signal;
    event.value = p_value;
    batch_resource_signals.push_back(event);
}

void AnimationMixer::queue_animation_signal(const StringName &p_signal, const StringName &p_animation) {
    if (batch_evaluating) { batch_signals.push_back(Pair<StringName, StringName>(p_signal, p_animation)); }
    else { call_deferred(SNAME("emit_signal"), p_signal, p_animation); }
}

bool AnimationTree::_prepare_batch_graph() {
    const bool changed = properties_dirty || validation_dirty || !batch_graph_checked;
    _update_properties();
    if (validation_dirty) { _update_connections(); validation_dirty = false; }
    if (root_animation_node.is_null()) { batch_fallback_reason = "Empty animation graph"; return false; }
    if (!changed) { batch_fallback_reason = batch_graph_reason; return batch_graph_reason.is_empty(); }
    batch_graph_checked = false;
    for (const KeyValue<StringName, AnimationNodeInstance> &kv : instance_map) {
        const Ref<AnimationNode> &node = kv.value.resource;
        if (node->get_script_instance()) { batch_fallback_reason = "Script animation node"; return false; }
        const StringName type = node->get_class_name();
        if (type != SNAME("AnimationNodeBlendTree") && type != SNAME("AnimationNodeStateMachine") &&
            type != SNAME("AnimationNodeAnimation") && type != SNAME("AnimationNodeTimeScale") &&
            type != SNAME("AnimationNodeTimeSeek") && type != SNAME("AnimationNodeBlend2") &&
            type != SNAME("AnimationNodeAdd2") && type != SNAME("AnimationNodeOneShot") &&
            type != SNAME("AnimationNodeBlendSpace1D") && type != SNAME("AnimationNodeBlendSpace2D") &&
            type != SNAME("AnimationNodeOutput") && type != SNAME("AnimationNodeStartState") && type != SNAME("AnimationNodeEndState")) {
            batch_fallback_reason = "Unsupported animation node: " + String(type); return false;
        }
        AnimationNodeStateMachine *machine = Object::cast_to<AnimationNodeStateMachine>(node.ptr());
        if (machine) {
            for (int i = 0; i < machine->get_transition_count(); ++i) {
                if (!machine->get_transition(i)->get_advance_expression().is_empty()) {
                    batch_fallback_reason = "Scene-dependent transition expression"; return false;
                }
            }
        }
        Variant *const *observer = kv.value.property_ptrs.getptr(SNAME("observer"));
        if (observer && (*observer)->get_type() == Variant::OBJECT && static_cast<Object *>(**observer) != nullptr) {
            batch_fallback_reason = "Animation observer requires synchronous evaluation"; return false;
        }
    }
    batch_graph_checked = true;
    batch_graph_reason = String();
    return true;
}
#endif
