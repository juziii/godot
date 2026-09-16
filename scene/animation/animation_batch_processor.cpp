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
    ClassDB::bind_method(D_METHOD("unregister_tree", "handle"), &AnimationBatchProcessor::unregister_tree);
    ClassDB::bind_method(D_METHOD("queue_update", "handle", "delta"), &AnimationBatchProcessor::queue_update);
    ClassDB::bind_method(D_METHOD("submit", "batch_size", "max_workers"), &AnimationBatchProcessor::submit, DEFVAL(8), DEFVAL(8));
    ClassDB::bind_method(D_METHOD("complete_batch", "handle"), &AnimationBatchProcessor::complete_batch);
    ClassDB::bind_method(D_METHOD("complete_and_publish"), &AnimationBatchProcessor::complete_and_publish);
    ClassDB::bind_method(D_METHOD("finish_all"), &AnimationBatchProcessor::finish_all);
    ClassDB::bind_method(D_METHOD("is_pending"), &AnimationBatchProcessor::is_pending);
    ClassDB::bind_method(D_METHOD("was_evaluated", "handle"), &AnimationBatchProcessor::was_evaluated);
    ClassDB::bind_method(D_METHOD("get_statistics"), &AnimationBatchProcessor::get_statistics);
    ClassDB::bind_method(D_METHOD("get_fallback_reason", "handle"), &AnimationBatchProcessor::get_fallback_reason);
}

int64_t AnimationBatchProcessor::register_tree(AnimationTree *p_tree, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods) {
    ERR_FAIL_COND_V(!Thread::is_main_thread(), 0);
    ERR_FAIL_COND_V(publishing, 0);
    complete_and_publish();
    ERR_FAIL_NULL_V(p_tree, 0);
    for (Entry *entry : entries) { if (entry->tree == p_tree) { return entry->handle; } }
    ERR_FAIL_COND_V(p_tree->batch_owner != nullptr, 0);
    Entry *entry = memnew(Entry);
    entry->handle = next_handle++;
    entry->tree = p_tree;
    entry->tree_id = p_tree->get_instance_id();
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
    p_tree->set_callback_mode_process(AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
    p_tree->batch_owner = this;
    p_tree->batch_bindings_checked = false;
    entries.push_back(entry);
    handles.insert(entry->handle, entry);
    return entry->handle;
}

void AnimationBatchProcessor::unregister_tree(int64_t p_handle) {
    ERR_FAIL_COND(!Thread::is_main_thread());
    complete_and_publish();
    Entry **found = handles.getptr(p_handle);
    if (!found) { return; }
    Entry *entry = *found;
    if (publishing) { entry->removed = true; return; }
    _remove_entry(entry);
}

void AnimationBatchProcessor::_remove_entry(Entry *entry) {
    if (ObjectDB::get_instance(entry->tree_id) == entry->tree) {
        entry->tree->batch_owner = nullptr;
        entry->tree->batch_poses.clear();
        entry->tree->batch_safe_methods.clear();
        entry->tree->set_callback_mode_process(entry->old_mode);
    }
    for (const Ref<SkeletonAnimationPose> &pose : entry->poses) { pose->release(); }
    handles.erase(entry->handle);
    entries.erase(entry);
    memdelete(entry);
}

bool AnimationBatchProcessor::queue_update(int64_t p_handle, double p_delta) {
    ERR_FAIL_COND_V(!Thread::is_main_thread() || pending || publishing, false);
    Entry **found = handles.getptr(p_handle);
    ERR_FAIL_NULL_V(found, false);
    Entry *entry = *found;
    ERR_FAIL_COND_V(entry->queued, false); // Distinct zero-step requests must not silently merge.
    entry->delta = p_delta;
    entry->queued = true;
    return true;
}

int64_t AnimationBatchProcessor::submit(int p_batch_size, int p_max_workers) {
    ERR_FAIL_COND_V(!Thread::is_main_thread() || pending || publishing, 0);
    ERR_FAIL_COND_V(p_batch_size < 1 || p_max_workers < 1, 0);
    GodotProfileZone("Animation.Prepare");
    work.clear();
    fallback_count = early_wait_count = failed_count = 0;
    for (Entry *entry : entries) {
        if (!entry->queued) { continue; }
        entry->queued = false;
        entry->published = false;
        entry->fallback = String();
        if (ObjectDB::get_instance(entry->tree_id) != entry->tree) { continue; }
        entry->prepared = true;
        for (const Ref<SkeletonAnimationPose> &pose : entry->poses) {
            if (!pose->capture_current()) {
                entry->prepared = false;
                entry->fallback = pose->get_fallback_reason();
                break;
            }
        }
        if (entry->prepared) {
            entry->prepared = entry->tree->_prepare_batch(entry->delta);
            if (!entry->prepared) { entry->fallback = entry->tree->batch_fallback_reason; }
        }
        if (!entry->prepared) { fallback_count++; }
        work.push_back(entry);
    }
    batch_count = (work.size() + p_batch_size - 1) / p_batch_size;
    while (batches.size() < uint32_t(batch_count)) { batches.push_back(memnew(Batch)); }
    for (int i = 0; i < batch_count; ++i) {
        Batch *batch = batches[i];
        while (batch->ready.try_wait()) {}
        batch->completed.store(false, std::memory_order_relaxed);
        batch->start = i * p_batch_size;
        batch->end = MIN(batch->start + p_batch_size, work.size());
    }
    frame_id++;
    pending = !work.is_empty();
    if (!pending) { worker_count = 0; return frame_id; }
    active_processors.push_back(this);
    worker_count = MIN(MIN(batch_count, p_max_workers), MIN(WorkerThreadPool::get_singleton()->get_thread_count(), MAX(1, OS::get_singleton()->get_processor_count() - 2)));
    if (work.size() < 16 || worker_count <= 1) {
        worker_count = 1;
        for (int i = 0; i < batch_count; ++i) { _execute_batch(i, false); }
    } else {
        GodotProfileZone("Animation.Launch");
        group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &AnimationBatchProcessor::_execute_batch, false, batch_count, worker_count, true, "Animation batches");
    }
    return frame_id;
}

void AnimationBatchProcessor::_execute_batch(uint32_t p_index, bool p_unused) {
    GodotProfileZone("Animation.Batch");
    Batch *batch = batches[p_index];
    for (uint32_t i = batch->start; i < batch->end; ++i) {
        Entry *entry = work[i];
        if (entry->prepared) { entry->tree->_evaluate_batch(); }
    }
    batch->completed.store(true, std::memory_order_release);
    batch->ready.post();
}

void AnimationBatchProcessor::_wait_batch(int p_index) {
    if (!batches[p_index]->completed.load(std::memory_order_acquire)) { batches[p_index]->ready.wait(); }
}

void AnimationBatchProcessor::_publish_entry(Entry *p_entry) {
    if (p_entry->published || p_entry->removed) { return; }
    p_entry->published = true;
    if (ObjectDB::get_instance(p_entry->tree_id) != p_entry->tree) { return; }
    if (p_entry->prepared) {
        if (!p_entry->tree->batch_succeeded) { failed_count++; }
        p_entry->tree->_publish_batch();
    } else {
        GodotProfileZone("Animation.Fallback");
        for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) { pose->release(); }
        p_entry->tree->advance(p_entry->delta);
    }
}

void AnimationBatchProcessor::complete_batch(int64_t p_handle) {
    ERR_FAIL_COND(!Thread::is_main_thread());
    if (!pending || publishing) { return; }
    int target_batch = -1;
    for (int b = 0; b < batch_count; ++b) {
        for (uint32_t i = batches[b]->start; i < batches[b]->end; ++i) {
            if (work[i]->handle == uint64_t(p_handle)) { target_batch = b; }
        }
    }
    if (target_batch < 0) { return; }
    early_wait_count++;
    // Readers use SkeletonAnimationPose; scene state and callbacks publish only at the ordered fence.
    { GodotProfileZone("Animation.Wait"); _wait_batch(target_batch); }
}

void AnimationBatchProcessor::complete_and_publish() {
    ERR_FAIL_COND(!Thread::is_main_thread());
    if (!pending || publishing) { return; }
    { GodotProfileZone("Animation.Wait");
        if (group != -1) {
            WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
            group = -1;
        }
    }
    pending = false;
    publishing = true;
    active_processors.erase(this);
    { GodotProfileZone("Animation.Publish"); for (Entry *entry : work) { _publish_entry(entry); } }
    publishing = false;
    for (int i = int(entries.size()) - 1; i >= 0; --i) {
        if (entries[i]->removed) { _remove_entry(entries[i]); }
    }
}

void AnimationBatchProcessor::finish_pending_frames() {
    while (!active_processors.is_empty()) { active_processors[0]->complete_and_publish(); }
}

Dictionary AnimationBatchProcessor::get_statistics() const {
    Dictionary result;
    result["frame"] = frame_id;
    result["pawns"] = work.size();
    result["batches"] = batch_count;
    result["workers"] = worker_count;
    result["fallbacks"] = fallback_count;
    result["early_waits"] = early_wait_count;
    result["failed"] = failed_count;
    return result;
}

String AnimationBatchProcessor::get_fallback_reason(int64_t p_handle) const {
    Entry *const *entry = handles.getptr(p_handle);
    return entry ? (*entry)->fallback : String("Invalid registration");
}

bool AnimationBatchProcessor::was_evaluated(int64_t p_handle) const {
    Entry *const *entry = handles.getptr(p_handle);
    return !pending && entry && (*entry)->prepared && ObjectDB::get_instance((*entry)->tree_id) == (*entry)->tree && (*entry)->tree->batch_succeeded;
}

AnimationBatchProcessor::~AnimationBatchProcessor() {
    finish_all();
    while (!entries.is_empty()) { unregister_tree(entries[0]->handle); }
    for (Batch *batch : batches) { memdelete(batch); }
}

void AnimationMixer::finish_pending_animation() {
    if (batch_owner && Thread::is_main_thread()) { batch_owner->finish_for_dependency(); }
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
    { GodotProfileZone("Animation.Init"); _blend_init(); }
    if (cache_valid && _blend_pre_process(batch_delta, track_count, track_map)) {
        { GodotProfileZone("Animation.Weights"); _blend_calc_total_weight(); }
        { GodotProfileZone("Animation.SampleBlend"); _blend_process(batch_delta, false); }
        _blend_apply();
        { GodotProfileZone("Animation.Skeleton"); for (BatchPoseBinding &binding : batch_poses) { binding.pose->evaluate(); } }
        batch_succeeded = true;
    }
    batch_evaluating = false;
}

void AnimationMixer::_publish_batch() {
    if (batch_succeeded) {
        { GodotProfileZone("Animation.Events");
            batch_event_pass = true;
            _blend_process(batch_delta, false);
            batch_event_pass = false;
        }
        for (BatchPoseBinding &binding : batch_poses) { binding.pose->publish(); }
        batch_publishing = true;
        _blend_apply();
        batch_publishing = false;
        _blend_post_process();
        emit_signal(SNAME("mixer_applied"));
        for (const BatchResourceSignal &event : batch_resource_signals) { event.target->emit_signal(event.signal, event.value); }
        for (const Pair<StringName, StringName> &event : batch_signals) { call_deferred(SNAME("emit_signal"), event.first, event.second); }
    }
    clear_animation_instances();
    batch_signals.clear();
    batch_method_events.clear();
    batch_audio_events.clear();
    batch_resource_signals.clear();
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
