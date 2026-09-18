#include "animation_batch_processor.h"

#ifndef _3D_DISABLED
#include "core/object/class_db.h"
#include "core/os/thread.h"
#include "core/profiling/profiling.h"

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

void AnimationMixer::_discard_batch() {
    // Tree evaluation appends animation instances on the live mixer. Without this
    // cleanup, a discarded frame is blended again after unregister/re-register.
    clear_animation_instances();
    batch_initial_instances.clear();
    batch_signals.clear();
    batch_method_events.clear();
    batch_audio_events.clear();
    batch_resource_signals.clear();
    batch_succeeded = false;
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
#endif
