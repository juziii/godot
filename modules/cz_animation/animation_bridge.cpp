/**************************************************************************/
/*  animation_bridge.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "animation_bridge.h"

#include "animation_batch_processor.h"

#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/profiling/profiling.h"
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/skeleton_modifier_3d.h"
#include "scene/animation/animation_blend_space_2d.h"
#include "scene/animation/animation_node_state_machine.h"

struct CZAnimationBridge::State {
	struct Instance {
		AnimationMixer::AnimationInstance value;
		LocalVector<real_t> weights;
		LocalVector<real_t> normalized_weights;
		bool zero_bone_weights_certified = false;
	};
	struct PoseBinding {
		Ref<SkeletonAnimationPose> pose;
		uint64_t layout = 0;
		uint64_t track_version = 0;
		bool tracks_valid = false;
		HashMap<Ref<Animation>, CZPoseTrackBinding> tracks;
		LocalVector<AnimationMixer::TrackCacheTransform *> transform_caches;
		LocalVector<uint8_t> bone_blend_mask;
		bool bone_blend_mask_eligible = false;
	};
	struct Signal {
		Ref<Resource> resource;
		StringName signal;
		StringName value;
	};
	AnimationMixer *mixer = nullptr;
	ObjectID id;
	ObjectID parent_id;
	AnimationBatchProcessor *owner = nullptr;
	LocalVector<PoseBinding> poses;
	LocalVector<Instance> instances;
	LocalVector<Instance> initial_instances;
	uint32_t instance_count = 0;
	uint32_t initial_instance_count = 0;
	LocalVector<Signal> signals;
	HashMap<StringName, BlendSpaceState> blend_spaces;
	HashSet<StringName> safe_methods;
	MethodValidationStatistics method_statistics;
	EvaluationStatistics evaluation_statistics;
	CZPoseTrackBinding::SamplingStatistics sampling_statistics;
	bool phase_diagnostics_enabled = false;
	bool pose_fastpaths_enabled = true;
	LocalVector<uint8_t> bone_blend_mask;
	bool bone_blend_mask_dirty = true;
	bool bone_blend_mask_eligible = false;
	bool bone_blend_mask_prepared = false;
	uint64_t method_track_version = 0;
	uint64_t method_animation_set_pass = 0;
	bool methods_checked = false;
	String method_failure;
	AnimationPlayer::Playback playback;
	StringName finished_animation;
	ObjectID from_animation;
	bool end_reached = false;
	bool end_notify = false;
	bool evaluating = false;
	bool publishing = false;
	bool success = false;
	double delta = 0;
	String reason;
};

static HashMap<ObjectID, CZAnimationBridge::State *> mixer_states;
static HashMap<const SkeletonAnimationPose *, CZAnimationBridge::State *> pose_states;
static Mutex state_owner_mutex;
struct ObservedSkeleton {
	int users = 0;
	int controls = 0;
	Skeleton3D::ModifierCallbackModeProcess saved_callback = Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_IDLE;
	uint64_t binding = 1;
	uint64_t input = 1;
};
static HashMap<ObjectID, ObservedSkeleton> skeleton_versions;
struct ObservedSkeletonLayout {
	int users = 0;
	uint64_t version = 1;
};
static HashMap<ObjectID, ObservedSkeletonLayout> skeleton_layout_versions;
static HashMap<ObjectID, bool> skeleton_modifier_switches;
static HashMap<ObjectID, HashMap<StringName, CZAnimationBridge::BlendSpaceState>> synchronous_blend_spaces;
static thread_local CZAnimationBridge::State *evaluation_state = nullptr;
static thread_local int player_mutation_depth = 0;

class CZAnimationEvaluationScope {
	CZAnimationBridge::State *state;
	CZAnimationBridge::State *previous;
	bool publishing;
	bool previous_flag;

public:
	CZAnimationEvaluationScope(CZAnimationBridge::State *p_state, bool p_publishing) :
			state(p_state), previous(evaluation_state), publishing(p_publishing), previous_flag(p_publishing ? p_state->publishing : p_state->evaluating) {
		evaluation_state = state;
		(publishing ? state->publishing : state->evaluating) = true;
	}

	~CZAnimationEvaluationScope() {
		(publishing ? state->publishing : state->evaluating) = previous_flag;
		evaluation_state = previous;
	}
};

CZAnimationBridge::State *CZAnimationBridge::create(AnimationMixer *p_mixer, AnimationBatchProcessor *p_owner, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, State *p_parent) {
	ERR_FAIL_NULL_V(p_mixer, nullptr);
	ERR_FAIL_NULL_V(p_owner, nullptr);
	ERR_FAIL_COND_V(mixer_states.has(p_mixer->get_instance_id()), nullptr);
	LocalVector<Ref<SkeletonAnimationPose>> poses;
	for (int i = 0; i < p_poses.size(); i++) {
		Ref<SkeletonAnimationPose> pose = p_poses[i];
		if (pose.is_null()) {
			continue;
		}
		for (const Ref<SkeletonAnimationPose> &other : poses) {
			ERR_FAIL_COND_V_MSG(other == pose || (pose->skeleton_id.is_valid() && other->skeleton_id == pose->skeleton_id), nullptr, "A registration cannot contain duplicate poses or skeletons.");
		}
		for (const KeyValue<ObjectID, State *> &state : mixer_states) {
			for (const State::PoseBinding &binding : state.value->poses) {
				ERR_FAIL_COND_V_MSG(binding.pose == pose || (pose->skeleton_id.is_valid() && binding.pose->skeleton_id == pose->skeleton_id), nullptr, "A pose or skeleton already belongs to another animation registration.");
			}
		}
		poses.push_back(pose);
	}
	State *state = memnew(State);
	state->mixer = p_mixer;
	state->id = p_mixer->get_instance_id();
	state->owner = p_owner;
	state->pose_fastpaths_enabled = p_owner->pose_fastpaths_enabled;
	state->parent_id = p_parent ? p_parent->id : ObjectID();
	state->poses.resize(poses.size());
	for (uint32_t i = 0; i < poses.size(); i++) {
		state->poses[i].pose = poses[i];
		pose_states.insert(poses[i].ptr(), state);
	}
	for (const String &method : p_safe_methods) {
		state->safe_methods.insert(StringName(method));
	}
	mixer_states.insert(state->id, state);
	return state;
}

void CZAnimationBridge::destroy(State *p_state) {
	if (!p_state) {
		return;
	}
	mixer_states.erase(p_state->id);
	for (const State::PoseBinding &binding : p_state->poses) {
		pose_states.erase(binding.pose.ptr());
	}
	memdelete(p_state);
}

void CZAnimationBridge::detach_owner(State *p_state) {
	MutexLock lock(state_owner_mutex);
	p_state->owner = nullptr;
}

void CZAnimationBridge::reset_method_validation_statistics(State *p_state) {
	p_state->method_statistics = MethodValidationStatistics();
}

CZAnimationBridge::MethodValidationStatistics CZAnimationBridge::get_method_validation_statistics(const State *p_state) {
	return p_state->method_statistics;
}

bool CZAnimationBridge::prepare(State *p_state, double p_delta, const LocalVector<Ref<SkeletonAnimationPose>> &p_poses) {
	AnimationMixer *mixer = p_state->mixer;
	// The processor retains itself and defers removed entries while preparing.
	// A callout can delete the Node, but cannot dispose this registration State.
	const ObjectID mixer_id = p_state->id;
	p_state->reason = String();
	p_state->success = false;
	p_state->bone_blend_mask_prepared = false;
	p_state->signals.clear();
	p_state->delta = p_delta;
	for (uint32_t index = 0; index < p_poses.size(); index++) {
		for (const Ref<SkeletonAnimationPose> &source : { p_poses[index]->copy_source, p_poses[index]->attachment_source }) {
			if (source.is_null()) {
				continue;
			}
			bool ordered = false;
			for (uint32_t prior = 0; prior < index; prior++) {
				ordered |= p_poses[prior] == source;
			}
			if (State **parent = mixer_states.getptr(p_state->parent_id)) {
				for (const State::PoseBinding &binding : (*parent)->poses) {
					ordered |= binding.pose == source;
				}
			}
			if (!ordered) {
				p_state->reason = "Pose source is not ordered before its consumer in this entry or its parent unit.";
				return false;
			}
		}
	}
	if (ObjectDB::get_instance(mixer_id) != mixer || mixer->get_script_instance() || mixer->capture_cache.animation.is_valid()) {
		p_state->reason = "Script mixer or capture animation requires synchronous evaluation.";
		return false;
	}
	if (mixer->get_class_name() != SNAME("AnimationTree") && mixer->get_class_name() != SNAME("AnimationPlayer")) {
		p_state->reason = "Custom native mixer post-processing requires synchronous evaluation.";
		return false;
	}
	if (AnimationPlayer *player = Object::cast_to<AnimationPlayer>(mixer)) {
		if (!player->playback.current.is_enabled) {
			p_state->reason = "Disabled player playback requires synchronous processing.";
			return false;
		}
	}
	if (!mixer->cache_valid && !mixer->_update_caches()) {
		p_state->reason = "Unresolved animation bindings.";
		return false;
	}
	if (ObjectDB::get_instance(mixer_id) != mixer) {
		p_state->reason = "Animation mixer was deleted during preparation.";
		return false;
	}
	// safe_methods is immutable for this registration. Check the whole library,
	// including inactive clips, once per binding/content revision. New clips can
	// invalidate cache_valid without _clear_caches(), so both versions matter.
	if (!p_state->methods_checked || p_state->method_track_version != mixer->track_map_version ||
			p_state->method_animation_set_pass != mixer->animation_set_update_pass) {
		p_state->method_statistics.scans++;
		p_state->method_failure = String();
		for (const KeyValue<StringName, AnimationMixer::AnimationData> &kv : mixer->animation_set) {
			p_state->method_statistics.animations++;
			const Ref<Animation> &animation = kv.value.animation;
			for (int track = 0; track < animation->get_track_count(); track++) {
				p_state->method_statistics.tracks++;
				if (animation->track_is_enabled(track) && animation->track_get_type(track) == Animation::TYPE_METHOD) {
					for (int key = 0; key < animation->track_get_key_count(track); key++) {
						p_state->method_statistics.keys++;
						if (!p_state->safe_methods.has(animation->method_track_get_name(track, key))) {
							p_state->method_failure = "Immediate method track requires synchronous evaluation.";
							break;
						}
					}
				}
				if (!p_state->method_failure.is_empty()) {
					break;
				}
			}
			if (!p_state->method_failure.is_empty()) {
				break;
			}
		}
		p_state->method_track_version = mixer->track_map_version;
		p_state->method_animation_set_pass = mixer->animation_set_update_pass;
		p_state->methods_checked = true;
	} else {
		p_state->method_statistics.cache_hits++;
	}
	if (!p_state->method_failure.is_empty()) {
		p_state->reason = p_state->method_failure;
		return false;
	}
	const auto certificate_current = [&]() {
		if (ObjectDB::get_instance(mixer_id) != mixer) {
			p_state->reason = "Animation mixer was deleted during preparation.";
			return false;
		}
		if (!mixer->cache_valid || !p_state->methods_checked || p_state->method_track_version != mixer->track_map_version ||
				p_state->method_animation_set_pass != mixer->animation_set_update_pass) {
			p_state->reason = "Animation bindings changed during preparation.";
			return false;
		}
		return true;
	};
	if (AnimationTree *tree = Object::cast_to<AnimationTree>(mixer)) {
		if (tree->properties_dirty || tree->validation_dirty) {
			p_state->blend_spaces.clear();
		}
		tree->_update_properties();
		if (!certificate_current()) {
			return false;
		}
		if (tree->validation_dirty) {
			tree->_update_connections();
			tree->validation_dirty = false;
		}
		LocalVector<Ref<AnimationNodeBlendSpace2D>> dirty_spaces;
		for (const KeyValue<StringName, AnimationNodeInstance> &kv : tree->instance_map) {
			if (AnimationNodeBlendSpace2D *space = Object::cast_to<AnimationNodeBlendSpace2D>(kv.value.resource.ptr())) {
				if (space->auto_triangles && space->triangles_dirty) {
					dirty_spaces.push_back(Ref<AnimationNodeBlendSpace2D>(space));
				}
			}
		}
		// triangles_updated can replace the graph. Keep resources alive and do
		// not retain an instance_map iterator across that arbitrary callout.
		for (const Ref<AnimationNodeBlendSpace2D> &space : dirty_spaces) {
			space->_update_triangles();
			if (!certificate_current()) {
				return false;
			}
		}
		tree->_update_properties();
		if (!certificate_current()) {
			return false;
		}
		if (tree->validation_dirty) {
			tree->_update_connections();
			tree->validation_dirty = false;
		}
		if (tree->root_animation_node.is_null()) {
			p_state->reason = "Empty animation graph.";
			return false;
		}
		for (const KeyValue<StringName, AnimationNodeInstance> &kv : tree->instance_map) {
			if (AnimationNodeBlendSpace2D *space = Object::cast_to<AnimationNodeBlendSpace2D>(kv.value.resource.ptr())) {
				if (space->auto_triangles && space->triangles_dirty) {
					p_state->reason = "Animation graph changed during triangle preparation.";
					return false;
				}
			}
			if (!p_state->blend_spaces.has(kv.key)) {
				p_state->blend_spaces.insert(kv.key, BlendSpaceState());
			}
			if (kv.value.resource->get_script_instance()) {
				p_state->reason = "Script animation node requires synchronous evaluation.";
				return false;
			}
			const StringName type = kv.value.resource->get_class_name();
			if (type != SNAME("AnimationNodeBlendTree") && type != SNAME("AnimationNodeStateMachine") &&
					type != SNAME("AnimationNodeAnimation") && type != SNAME("AnimationNodeTimeScale") &&
					type != SNAME("AnimationNodeTimeSeek") && type != SNAME("AnimationNodeBlend2") &&
					type != SNAME("AnimationNodeAdd2") && type != SNAME("AnimationNodeOneShot") &&
					type != SNAME("AnimationNodeBlendSpace1D") && type != SNAME("AnimationNodeBlendSpace2D") &&
					type != SNAME("AnimationNodeOutput") && type != SNAME("AnimationNodeStartState") && type != SNAME("AnimationNodeEndState")) {
				p_state->reason = "Unsupported animation node: " + String(type);
				return false;
			}
			if (AnimationNodeStateMachine *machine = Object::cast_to<AnimationNodeStateMachine>(kv.value.resource.ptr())) {
				for (int i = 0; i < machine->get_transition_count(); i++) {
					if (!machine->get_transition(i)->get_advance_expression().is_empty()) {
						p_state->reason = "Scene-dependent transition expression.";
						return false;
					}
				}
			}
			Variant *const *observer = kv.value.property_ptrs.getptr(SNAME("observer"));
			if (observer && (*observer)->get_type() == Variant::OBJECT && static_cast<Object *>(**observer)) {
				p_state->reason = "Animation observer requires synchronous evaluation.";
				return false;
			}
		}
	}
	if (p_state->poses.size() != p_poses.size()) {
		p_state->bone_blend_mask_dirty = true;
		p_state->poses.clear();
		p_state->poses.resize(p_poses.size());
	}
	for (uint32_t index = 0; index < p_poses.size(); index++) {
		State::PoseBinding &binding = p_state->poses[index];
		const Ref<SkeletonAnimationPose> &pose = p_poses[index];
		if (!binding.tracks_valid || binding.pose != pose || binding.layout != pose->layout_version || binding.track_version != mixer->track_map_version) {
			p_state->bone_blend_mask_dirty = true;
			binding.tracks_valid = false;
			binding.pose = pose;
			binding.layout = pose->layout_version;
			binding.tracks.clear();
			binding.transform_caches.clear();
			binding.bone_blend_mask_eligible = p_state->pose_fastpaths_enabled && mixer->deterministic;
			if (binding.bone_blend_mask_eligible) {
				binding.bone_blend_mask.resize(mixer->track_count);
				for (uint8_t &mask : binding.bone_blend_mask) {
					mask = 0;
				}
			}
			for (const KeyValue<Animation::TrackCacheID, AnimationMixer::TrackCache *> &entry : mixer->track_cache) {
				if (entry.value->type == Animation::TYPE_POSITION_3D && entry.value->path != mixer->root_motion_track) {
					auto *transform = static_cast<AnimationMixer::TrackCacheTransform *>(entry.value);
					if (transform->skeleton_id == pose->skeleton_id) {
						binding.transform_caches.push_back(transform);
					}
				}
			}
			for (const KeyValue<StringName, AnimationMixer::AnimationData> &kv : mixer->animation_set) {
				const Ref<Animation> &animation = kv.value.animation;
				CZPoseTrackBinding &tracks = binding.tracks[animation];
				tracks.reset(animation, pose->lod.get_active_bone_count());
				HashSet<int> channels;
				const LocalVector<AnimationMixer::TrackCache *> *mapping = mixer->animation_track_num_to_track_cache.getptr(animation);
				if (!mapping) {
					continue;
				}
				for (int track = 0; track < animation->get_track_count(); track++) {
					AnimationMixer::TrackCache *cache = (*mapping)[track];
					if (!cache || !animation->track_is_enabled(track)) {
						continue;
					}
					if (cache->type != Animation::TYPE_POSITION_3D) {
						if (cache->type == Animation::TYPE_VALUE || cache->type == Animation::TYPE_BEZIER) {
							Object *target = ObjectDB::get_instance(cache->object_id);
							// Continuous, built-in Node3D values are pure cache calculations
							// until the official main-thread _blend_apply(). Discrete or
							// user-defined setters keep the synchronous fallback.
							if (target && target->get_class_name() == SNAME("Node3D") && !target->get_script_instance() &&
									(animation->track_get_type(track) != Animation::TYPE_VALUE || animation->value_track_get_update_mode(track) != Animation::UPDATE_DISCRETE)) {
								continue;
							}
						}
						if (cache->type != Animation::TYPE_METHOD && cache->type != Animation::TYPE_AUDIO && cache->type != Animation::TYPE_BLEND_SHAPE) {
							p_state->reason = "Property or nested animation track requires synchronous evaluation.";
							return false;
						}
						continue;
					}
					if (animation->track_get_path(track) == mixer->root_motion_track) {
						continue;
					}
					AnimationMixer::TrackCacheTransform *transform = static_cast<AnimationMixer::TrackCacheTransform *>(cache);
					if (transform->skeleton_id != pose->skeleton_id) {
						bool found = false;
						for (const Ref<SkeletonAnimationPose> &other : p_poses) {
							found |= other->skeleton_id == transform->skeleton_id;
						}
						if (!found) {
							p_state->reason = "Transform outside registered skeletons.";
							return false;
						}
						continue;
					}
					const int compact = pose->lod.to_compact(transform->bone_idx);
					if (compact < 0 || !pose->bones[transform->bone_idx].enabled) {
						continue;
					}
					const int channel = compact * 3 + int(animation->track_get_type(track));
					if (channels.has(channel)) {
						p_state->reason = "Duplicate bone transform channel requires synchronous evaluation.";
						return false;
					}
					channels.insert(channel);
					tracks.add_track(animation->track_get_type(track), track, compact, cache->blend_idx);
					if (p_state->pose_fastpaths_enabled && mixer->deterministic) {
						if (cache->blend_idx >= 0 && cache->blend_idx < mixer->track_count) {
							binding.bone_blend_mask[cache->blend_idx] = 1;
						} else {
							binding.bone_blend_mask_eligible = false;
						}
					}
					switch (animation->track_get_type(track)) {
						case Animation::TYPE_POSITION_3D:
							pose->reference_pose.set_position(compact, transform->init_loc);
							pose->reference_mask[compact] |= 1;
							break;
						case Animation::TYPE_ROTATION_3D:
							pose->reference_pose.set_rotation(compact, transform->init_rot);
							pose->reference_mask[compact] |= 2;
							break;
						case Animation::TYPE_SCALE_3D:
							pose->reference_pose.set_scale(compact, transform->init_scale);
							pose->reference_mask[compact] |= 4;
							break;
						default:
							break;
					}
				}
			}
			binding.track_version = mixer->track_map_version;
			binding.tracks_valid = true;
		}
	}
	if (p_state->pose_fastpaths_enabled && mixer->deterministic &&
			(p_state->bone_blend_mask_dirty || p_state->bone_blend_mask.size() != uint32_t(mixer->track_count))) {
		p_state->bone_blend_mask.resize(mixer->track_count);
		for (uint8_t &mask : p_state->bone_blend_mask) {
			mask = 0;
		}
		p_state->bone_blend_mask_eligible = true;
		for (const State::PoseBinding &binding : p_state->poses) {
			if (!binding.tracks_valid || !binding.bone_blend_mask_eligible || binding.bone_blend_mask.size() != p_state->bone_blend_mask.size()) {
				p_state->bone_blend_mask_eligible = false;
				break;
			}
			for (uint32_t index = 0; index < binding.bone_blend_mask.size(); index++) {
				p_state->bone_blend_mask[index] |= binding.bone_blend_mask[index];
			}
		}
		p_state->bone_blend_mask_dirty = false;
		if (p_state->phase_diagnostics_enabled) {
			p_state->evaluation_statistics.zero_weight_mask_rebuilds++;
		}
	}
	const bool current = certificate_current();
	p_state->bone_blend_mask_prepared = current;
	return current;
}

void CZAnimationBridge::reset_evaluation_statistics(State *p_state, bool p_enabled) {
	p_state->phase_diagnostics_enabled = p_enabled;
	p_state->evaluation_statistics = EvaluationStatistics();
	p_state->sampling_statistics = CZPoseTrackBinding::SamplingStatistics();
}

CZAnimationBridge::EvaluationStatistics CZAnimationBridge::get_evaluation_statistics(const State *p_state) {
	EvaluationStatistics statistics = p_state->evaluation_statistics;
	statistics.sample_calls = p_state->sampling_statistics.calls;
	statistics.sample_zero_weight_calls = p_state->sampling_statistics.zero_weight_calls;
	statistics.sample_no_written_calls = p_state->sampling_statistics.no_written_calls;
	statistics.sample_empty_binding_calls = p_state->sampling_statistics.empty_binding_calls;
	statistics.sample_certified_zero_weight_calls = p_state->sampling_statistics.certified_zero_weight_calls;
	return statistics;
}

void CZAnimationBridge::evaluate(State *p_state) {
	// Construct before the TLS scope so its destruction is included in total_us.
	struct Measurement {
		State *state;
		uint64_t start;
		explicit Measurement(State *p_state) :
				state(p_state->phase_diagnostics_enabled ? p_state : nullptr),
				start(state ? OS::get_singleton()->get_ticks_usec() : 0) {}
		~Measurement() {
			if (state) {
				state->evaluation_statistics.total_us += OS::get_singleton()->get_ticks_usec() - start;
				state->evaluation_statistics.calls++;
			}
		}
	} measurement(p_state);
	const bool diagnostics = p_state->phase_diagnostics_enabled;
	AnimationMixer *mixer = p_state->mixer;
	AnimationPlayer *player = Object::cast_to<AnimationPlayer>(mixer);
	CZAnimationEvaluationScope scope(p_state, false);
	p_state->initial_instance_count = 0;
	p_state->instance_count = 0;
	const bool certify_zero_weights = p_state->pose_fastpaths_enabled && mixer->deterministic &&
			p_state->bone_blend_mask_prepared && !p_state->bone_blend_mask_dirty && p_state->bone_blend_mask_eligible && p_state->bone_blend_mask.size() == uint32_t(mixer->track_count);
	const auto snapshot_weights = [&](State::Instance &r_snapshot, const AnimationMixer::AnimationInstance &p_instance) {
		r_snapshot.value = p_instance;
		r_snapshot.weights.resize(mixer->track_count);
		const LocalVector<real_t> *source = p_instance.playback_info.track_weights;
		r_snapshot.zero_bone_weights_certified = certify_zero_weights && Math::is_finite(p_instance.playback_info.weight);
		int track = 0;
		if (r_snapshot.zero_bone_weights_certified) {
			for (; track < mixer->track_count; track++) {
				r_snapshot.weights[track] = source && track < int(source->size()) ? (*source)[track] : 1;
				if (p_state->bone_blend_mask[track] && r_snapshot.weights[track] != real_t(0)) {
					// Exact +/-0 only. Near-zero, NaN and infinity retain sampling's
					// original multiplication/threshold/interpolation order.
					r_snapshot.zero_bone_weights_certified = false;
					track++;
					break;
				}
			}
		}
		// Disabled/non-deterministic paths, or the remainder after one failed
		// certificate element, use the original copy without per-element tests.
		for (; track < mixer->track_count; track++) {
			r_snapshot.weights[track] = source && track < int(source->size()) ? (*source)[track] : 1;
		}
		r_snapshot.value.playback_info.track_weights = &r_snapshot.weights;
		if (diagnostics) {
			p_state->evaluation_statistics.raw_weight_elements += mixer->track_count;
		}
	};
	AnimationPlayer::Playback original_playback;
	if (player) {
		original_playback = player->playback;
		// The first key has its own zero-delta event pass. The actual frame still
		// advances by delta, matching AnimationPlayer's synchronous start behavior.
		if (player->playback.started) {
			if (diagnostics) {
				p_state->evaluation_statistics.first_key_calls++;
			}
			mixer->_blend_init();
			player->_blend_pre_process(0, mixer->track_count, mixer->track_map);
			p_state->initial_instance_count = mixer->animation_instances.size();
			if (p_state->initial_instances.size() < p_state->initial_instance_count) {
				p_state->initial_instances.resize(p_state->initial_instance_count);
			}
			for (uint32_t i = 0; i < mixer->animation_instances.size(); i++) {
				snapshot_weights(p_state->initial_instances[i], mixer->animation_instances[i]);
			}
			mixer->clear_animation_instances();
		}
	}
	uint64_t phase_start = diagnostics ? OS::get_singleton()->get_ticks_usec() : 0;
	mixer->_blend_init();
	const bool processed = mixer->cache_valid && mixer->_blend_pre_process(p_state->delta, mixer->track_count, mixer->track_map);
	if (processed) {
		mixer->_blend_calc_total_weight();
		if (diagnostics) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			p_state->evaluation_statistics.graph_us += now - phase_start;
			phase_start = now;
		}
		p_state->instance_count = mixer->animation_instances.size();
		if (diagnostics) {
			p_state->evaluation_statistics.animation_instances += p_state->instance_count;
			p_state->evaluation_statistics.track_count_sum += mixer->track_count;
		}
		if (p_state->instances.size() < p_state->instance_count) {
			p_state->instances.resize(p_state->instance_count);
		}
		for (uint32_t i = 0; i < mixer->animation_instances.size(); i++) {
			State::Instance &snapshot = p_state->instances[i];
			snapshot_weights(snapshot, mixer->animation_instances[i]);
			if (!p_state->pose_fastpaths_enabled || !mixer->deterministic) {
				snapshot.normalized_weights.resize(mixer->track_count);
				for (int track = 0; track < mixer->track_count; track++) {
					snapshot.normalized_weights[track] = snapshot.weights[track];
				}
				if (diagnostics) {
					p_state->evaluation_statistics.normalized_copy_elements += mixer->track_count;
				}
			}
			if (!mixer->deterministic) {
				for (const KeyValue<Animation::TrackCacheID, AnimationMixer::TrackCache *> &kv : mixer->track_cache) {
					const int track = kv.value->blend_idx;
					if (track >= 0 && track < mixer->track_count) {
						snapshot.normalized_weights[track] = Math::is_zero_approx(kv.value->total_weight) ? 0 : snapshot.weights[track] / kv.value->total_weight;
					}
				}
			}
			snapshot.value.playback_info.track_weights = &snapshot.weights;
			mixer->animation_instances[i].playback_info.track_weights = &snapshot.weights;
		}
		if (diagnostics) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			p_state->evaluation_statistics.weights_us += now - phase_start;
			phase_start = now;
		}
		for (State::PoseBinding &binding : p_state->poses) {
			SkeletonAnimationPose *pose = binding.pose.ptr();
			if (pose->sample_requested) {
				const uint64_t start = OS::get_singleton()->get_ticks_usec();
				{
					GodotProfileZone("CZAnimation.SampleBlend");
					pose->blended.copy_from(pose->reference_pose);
					for (uint32_t i = 0; i < p_state->instance_count; i++) {
						State::Instance &snapshot = p_state->instances[i];
						CZPoseTrackBinding *tracks = binding.tracks.getptr(snapshot.value.animation);
						if (tracks) {
							const LocalVector<real_t> *sample_weights = p_state->pose_fastpaths_enabled && mixer->deterministic ? &snapshot.weights : &snapshot.normalized_weights;
							tracks->sample_and_blend(snapshot.value.playback_info.time, snapshot.value.playback_info.weight,
									sample_weights, pose->motion_scale, pose->reference_pose, pose->sampled, pose->blended, pose->simd_enabled,
									p_state->pose_fastpaths_enabled, diagnostics ? &p_state->sampling_statistics : nullptr, snapshot.zero_bone_weights_certified);
						}
					}
					if (!mixer->deterministic) {
						for (const AnimationMixer::TrackCacheTransform *cache : binding.transform_caches) {
							const int compact = pose->lod.to_compact(cache->bone_idx);
							if (compact >= 0 && Math::is_zero_approx(cache->total_weight)) {
								// The official non-deterministic path skips the entire
								// transform cache, retaining the last public base pose.
								// Never retain a private final/modifier result here.
								pose->blended.set_position(compact, pose->retained_base.get_position(compact));
								pose->blended.set_rotation(compact, pose->retained_base.get_rotation(compact));
								pose->blended.set_scale(compact, pose->retained_base.get_scale(compact));
							}
						}
					}
				}
				pose->sample_blend_us += OS::get_singleton()->get_ticks_usec() - start;
				pose->evaluate();
			} else {
				pose->interpolate_frame();
			}
		}
		if (diagnostics) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			p_state->evaluation_statistics.pose_us += now - phase_start;
			phase_start = now;
		}
		// Bone tracks were sampled directly into SoA. Official root motion and
		// BlendShape caches still use their original resource sampling semantics.
		mixer->_blend_process(p_state->delta);
		if (diagnostics) {
			p_state->evaluation_statistics.aux_blend_us += OS::get_singleton()->get_ticks_usec() - phase_start;
		}
		p_state->success = true;
	} else if (diagnostics) {
		p_state->evaluation_statistics.graph_us += OS::get_singleton()->get_ticks_usec() - phase_start;
	}
	if (player) {
		p_state->playback = player->playback;
		p_state->finished_animation = player->finished_anim;
		p_state->from_animation = player->tmp_from;
		p_state->end_reached = player->end_reached;
		p_state->end_notify = player->end_notify;
		player->playback = original_playback;
	}
}

bool CZAnimationBridge::publish(State *p_state) {
	AnimationMixer *mixer = p_state->mixer;
	const ObjectID id = p_state->id;
	if (!p_state->success || ObjectDB::get_instance(id) != mixer) {
		return false;
	}
	if (AnimationPlayer *player = Object::cast_to<AnimationPlayer>(mixer)) {
		player->playback = p_state->playback;
		player->finished_anim = p_state->finished_animation;
		player->tmp_from = p_state->from_animation;
		player->end_reached = p_state->end_reached;
		player->end_notify = p_state->end_notify;
	}
	CZAnimationEvaluationScope scope(p_state, true);
	// Reuse the official event traversal on the main thread. Both first-key and
	// frame snapshots own their weight storage until this pass finishes.
	mixer->animation_instances.clear();
	for (uint32_t i = 0; i < p_state->initial_instance_count; i++) {
		mixer->animation_instances.push_back(p_state->initial_instances[i].value);
	}
	for (uint32_t i = 0; i < p_state->instance_count; i++) {
		mixer->animation_instances.push_back(p_state->instances[i].value);
	}
	mixer->_blend_process(p_state->delta);
	if (ObjectDB::get_instance(id) != mixer) {
		return false;
	}
	if (!mixer->cache_valid) {
		p_state->success = false;
	} else {
		for (State::PoseBinding &binding : p_state->poses) {
			if (binding.pose->display_requested && !binding.pose->publish()) {
				p_state->success = false;
				break;
			}
			if (ObjectDB::get_instance(id) != mixer) {
				return false;
			}
		}
		if (p_state->success) {
			mixer->_blend_apply();
			mixer->_blend_post_process();
			if (ObjectDB::get_instance(id) == mixer) {
				mixer->emit_signal(SNAME("mixer_applied"));
			}
			for (const State::Signal &signal : p_state->signals) {
				if (ObjectDB::get_instance(id) != mixer) {
					break;
				}
				if (signal.resource.is_valid()) {
					signal.resource->emit_signal(signal.signal, signal.value);
				} else {
					mixer->call_deferred(SNAME("emit_signal"), signal.signal, signal.value);
				}
			}
		}
	}
	if (ObjectDB::get_instance(id) == mixer) {
		mixer->clear_animation_instances();
	}
	return p_state->success;
}

void CZAnimationBridge::discard(State *p_state) {
	if (ObjectDB::get_instance(p_state->id) == p_state->mixer) {
		p_state->mixer->clear_animation_instances();
	}
	p_state->signals.clear();
	p_state->instance_count = 0;
	p_state->initial_instance_count = 0;
	p_state->success = false;
}

bool CZAnimationBridge::succeeded(const State *p_state) {
	return p_state->success;
}
String CZAnimationBridge::fallback(const State *p_state) {
	return p_state->reason;
}
AnimationMixer *CZAnimationBridge::mixer(State *p_state) {
	return ObjectDB::get_instance(p_state->id) == p_state->mixer ? p_state->mixer : nullptr;
}

void CZAnimationBridge::set_callback_mode(AnimationMixer *p_mixer, AnimationMixer::AnimationCallbackModeProcess p_mode) {
	if (p_mixer->callback_mode_process == p_mode) {
		return;
	}
	// The public setter toggles active, clears caches and restarts AnimationTree.
	// Scheduling ownership must only change which native process callback runs.
	const bool processing = p_mixer->processing;
	p_mixer->_set_process(false, true);
	p_mixer->callback_mode_process = p_mode;
	p_mixer->_set_process(processing, true);
}

void CZAnimationBridge::synchronize(AnimationMixer *p_mixer) {
	if (!Thread::is_main_thread() || (evaluation_state && evaluation_state->evaluating)) {
		return;
	}
	if (!p_mixer) {
		if (player_mutation_depth == 0) {
			AnimationBatchProcessor::finish_pending_frames();
		}
		return;
	}
	State **state = mixer_states.getptr(p_mixer->get_instance_id());
	Ref<AnimationBatchProcessor> owner;
	{
		MutexLock lock(state_owner_mutex);
		if (state && (*state)->owner && !(*state)->publishing) {
			// A concurrent finalizer may have brought the owner to reference count
			// zero, but cannot finish detaching/freeing it while this lock is held.
			owner = Ref<AnimationBatchProcessor>((*state)->owner);
		}
	}
	if (owner.is_valid()) {
		owner->finish_for_dependency(p_mixer);
	}
}

void CZAnimationBridge::synchronize_resource() {
	// Resource construction on a worker must not wait for its own group. A
	// library mutation on the scene thread can affect several mixers, including
	// inside a Player mutation scope, so it cannot use that scope's global skip.
	if (!Thread::is_main_thread() || (evaluation_state && evaluation_state->evaluating)) {
		return;
	}
	AnimationBatchProcessor::finish_pending_frames();
}

void CZAnimationBridge::begin_player_mutation(AnimationMixer *p_mixer) {
	synchronize(p_mixer);
	player_mutation_depth++;
}

void CZAnimationBridge::end_player_mutation() {
	player_mutation_depth--;
}

bool CZAnimationBridge::is_evaluating(const AnimationMixer *p_mixer) {
	return evaluation_state && evaluation_state->mixer == p_mixer && evaluation_state->evaluating;
}

bool CZAnimationBridge::is_processing_pose(const SkeletonAnimationPose *p_pose) {
	if (evaluation_state && (evaluation_state->evaluating || evaluation_state->publishing)) {
		for (const State::PoseBinding &binding : evaluation_state->poses) {
			if (binding.pose.ptr() == p_pose) {
				return true;
			}
		}
	}
	return false;
}

void CZAnimationBridge::complete_pose(const SkeletonAnimationPose *p_pose) {
	if (!Thread::is_main_thread() || is_processing_pose(p_pose)) {
		return;
	}
	State **state = pose_states.getptr(p_pose);
	Ref<AnimationBatchProcessor> owner;
	{
		MutexLock lock(state_owner_mutex);
		if (state && (*state)->owner) {
			owner = Ref<AnimationBatchProcessor>((*state)->owner);
		}
	}
	if (owner.is_valid()) {
		// Queries wait for their producer without publishing its events.
		owner->complete();
	}
}

bool CZAnimationBridge::skip_track(const AnimationMixer *p_mixer, Animation::TrackType p_type, bool p_root_motion) {
	if (!evaluation_state || evaluation_state->mixer != p_mixer) {
		return false;
	}
	if (evaluation_state->publishing) {
		return p_type != Animation::TYPE_METHOD && p_type != Animation::TYPE_AUDIO;
	}
	return p_type == Animation::TYPE_METHOD || p_type == Animation::TYPE_AUDIO ||
			((p_type == Animation::TYPE_POSITION_3D || p_type == Animation::TYPE_ROTATION_3D || p_type == Animation::TYPE_SCALE_3D) && !p_root_motion);
}

bool CZAnimationBridge::can_skip_tracks_early(const AnimationMixer *p_mixer) {
	return evaluation_state && evaluation_state->mixer == p_mixer && evaluation_state->pose_fastpaths_enabled;
}

bool CZAnimationBridge::skip_apply(const AnimationMixer *p_mixer, Animation::TrackType p_type, bool p_root_motion) {
	return evaluation_state && evaluation_state->mixer == p_mixer && evaluation_state->publishing && p_type == Animation::TYPE_POSITION_3D && !p_root_motion;
}

bool CZAnimationBridge::modifier_applies_influence(const SkeletonModifier3D *p_modifier) {
	return Object::cast_to<BoneRotationOffsetModifier3D>(p_modifier) != nullptr;
}

bool CZAnimationBridge::queue_animation_signal(AnimationMixer *p_mixer, const StringName &p_signal, const StringName &p_animation) {
	if (!is_evaluating(p_mixer)) {
		return false;
	}
	evaluation_state->signals.push_back({ Ref<Resource>(), p_signal, p_animation });
	return true;
}

bool CZAnimationBridge::queue_resource_signal(const Ref<Resource> &p_resource, const StringName &p_signal, const StringName &p_value) {
	if (!evaluation_state || !evaluation_state->evaluating) {
		return false;
	}
	evaluation_state->signals.push_back({ p_resource, p_signal, p_value });
	return true;
}

bool CZAnimationBridge::set_parameter_group(AnimationTree *p_tree, const TypedArray<StringName> &p_names, const Variant &p_value) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	ERR_FAIL_NULL_V(p_tree, false);
	const ObjectID id = p_tree->get_instance_id();
	synchronize(p_tree);
	p_tree = ObjectDB::get_instance<AnimationTree>(id);
	ERR_FAIL_NULL_V(p_tree, false);
	p_tree->_update_properties();
	for (int i = 0; i < p_names.size(); i++) {
		const Pair<Variant, bool> *property = p_tree->property_map.getptr(p_names[i]);
		ERR_FAIL_NULL_V_MSG(property, false, "Unknown AnimationTree parameter.");
		ERR_FAIL_COND_V_MSG(p_tree->is_inside_tree() && property->second, false, "Read-only AnimationTree parameter.");
		Variant converted = p_value;
		ERR_FAIL_COND_V_MSG(!Animation::validate_type_match(property->first, converted), false, "AnimationTree parameter type mismatch.");
	}
	for (int i = 0; i < p_names.size(); i++) {
		Variant converted = p_value;
		const Pair<Variant, bool> *property = p_tree->property_map.getptr(p_names[i]);
		Animation::validate_type_match(property->first, converted);
		p_tree->_set(p_names[i], converted);
	}
	return true;
}

uint64_t CZAnimationBridge::skeleton_version(const Skeleton3D *p_skeleton) {
	const ObservedSkeletonLayout *layout = skeleton_layout_versions.getptr(p_skeleton->get_instance_id());
	if (layout) {
		return layout->version;
	}
	const ObservedSkeleton *version = skeleton_versions.getptr(p_skeleton->get_instance_id());
	return version ? version->binding : 1;
}

uint64_t CZAnimationBridge::skeleton_input_version(const Skeleton3D *p_skeleton) {
	const ObservedSkeleton *version = skeleton_versions.getptr(p_skeleton->get_instance_id());
	return version ? version->input : 1;
}

void CZAnimationBridge::observe_skeleton(Skeleton3D *p_skeleton) {
	ObservedSkeleton &version = skeleton_versions[p_skeleton->get_instance_id()];
	if (version.users++ == 0) {
		if (const ObservedSkeletonLayout *layout = skeleton_layout_versions.getptr(p_skeleton->get_instance_id())) {
			version.binding = layout->version;
		}
	}
}

void CZAnimationBridge::observe_skeleton_layout(Skeleton3D *p_skeleton) {
	ObservedSkeletonLayout &layout = skeleton_layout_versions[p_skeleton->get_instance_id()];
	if (layout.users++ == 0) {
		if (const ObservedSkeleton *version = skeleton_versions.getptr(p_skeleton->get_instance_id())) {
			layout.version = version->binding;
		}
	}
}

void CZAnimationBridge::unobserve_skeleton_layout(ObjectID p_id) {
	ObservedSkeletonLayout *layout = skeleton_layout_versions.getptr(p_id);
	if (layout && --layout->users == 0) {
		skeleton_layout_versions.erase(p_id);
	}
}

void CZAnimationBridge::unobserve_skeleton(ObjectID p_id) {
	ObservedSkeleton *version = skeleton_versions.getptr(p_id);
	if (version && --version->users == 0) {
		skeleton_versions.erase(p_id);
	}
}

void CZAnimationBridge::acquire_skeleton_control(Skeleton3D *p_skeleton) {
	ObservedSkeleton &version = skeleton_versions[p_skeleton->get_instance_id()];
	if (version.controls++ == 0) {
		version.saved_callback = p_skeleton->get_modifier_callback_mode_process();
		p_skeleton->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
	}
}

void CZAnimationBridge::release_skeleton_control(ObjectID p_id) {
	ObservedSkeleton *version = skeleton_versions.getptr(p_id);
	if (version && version->controls > 0 && --version->controls == 0) {
		if (Skeleton3D *skeleton = ObjectDB::get_instance<Skeleton3D>(p_id)) {
			skeleton->set_modifier_callback_mode_process(version->saved_callback);
		}
	}
}

void CZAnimationBridge::invalidate_skeleton(Skeleton3D *p_skeleton, bool p_binding) {
	if (!Thread::is_main_thread() || (skeleton_versions.is_empty() && (!p_binding || skeleton_layout_versions.is_empty()))) {
		return;
	}
	const ObjectID id = p_skeleton->get_instance_id();
	if (!skeleton_versions.has(id) && (!p_binding || !skeleton_layout_versions.has(id))) {
		return;
	}
	synchronize();
	ObservedSkeleton *version = skeleton_versions.getptr(id);
	if (version) {
		version->input++;
		if (p_binding) {
			version->binding++;
		}
	}
	if (p_binding) {
		if (ObservedSkeletonLayout *layout = skeleton_layout_versions.getptr(id)) {
			layout->version++;
		}
	}
}

void CZAnimationBridge::invalidate_binding_cache(AnimationMixer *p_mixer) {
	State **state = mixer_states.getptr(p_mixer->get_instance_id());
	if (state) {
		(*state)->methods_checked = false;
		(*state)->bone_blend_mask_dirty = true;
		(*state)->bone_blend_mask_eligible = false;
		(*state)->bone_blend_mask_prepared = false;
		for (State::PoseBinding &binding : (*state)->poses) {
			binding.track_version = 0;
			binding.tracks_valid = false;
			binding.tracks.clear();
			binding.transform_caches.clear();
			for (uint8_t &mask : binding.pose->reference_mask) {
				mask = 0;
			}
			binding.pose->reference_capture_dirty = true;
			binding.pose->history_count = 0;
		}
		for (KeyValue<StringName, BlendSpaceState> &space : (*state)->blend_spaces) {
			space.value.lengths.clear();
		}
	}
	if (HashMap<StringName, BlendSpaceState> *spaces = synchronous_blend_spaces.getptr(p_mixer->get_instance_id())) {
		for (KeyValue<StringName, BlendSpaceState> &space : *spaces) {
			space.value.lengths.clear();
		}
	}
}

void CZAnimationBridge::set_modifiers_enabled(Skeleton3D *p_skeleton, bool p_enabled) {
	invalidate_skeleton(p_skeleton);
	skeleton_modifier_switches[p_skeleton->get_instance_id()] = p_enabled;
	if (!p_enabled) {
		p_skeleton->update_delta = 0;
	}
}

bool CZAnimationBridge::modifiers_enabled(const Skeleton3D *p_skeleton) {
	const bool *enabled = skeleton_modifier_switches.getptr(p_skeleton->get_instance_id());
	return !enabled || *enabled;
}

void CZAnimationBridge::forget_object(ObjectID p_id) {
	skeleton_versions.erase(p_id);
	skeleton_layout_versions.erase(p_id);
	skeleton_modifier_switches.erase(p_id);
	synchronous_blend_spaces.erase(p_id);
}

void CZAnimationBridge::shutdown() {
	AnimationBatchProcessor::shutdown();
	LocalVector<Ref<AnimationBatchProcessor>> owners;
	{
		MutexLock lock(state_owner_mutex);
		for (const KeyValue<ObjectID, State *> &entry : mixer_states) {
			if (entry.value->owner) {
				Ref<AnimationBatchProcessor> owner(entry.value->owner);
				if (owner.is_valid() && !owners.has(owner)) {
					owners.push_back(owner);
				}
			}
		}
	}
	for (const Ref<AnimationBatchProcessor> &owner : owners) {
		while (!owner->entries.is_empty()) {
			owner->_remove(owner->entries[owner->entries.size() - 1]);
		}
	}
	AnimationBatchProcessor::drain_deferred_cleanup();
	skeleton_versions.clear();
	pose_states.clear();
	skeleton_layout_versions.clear();
	skeleton_modifier_switches.clear();
	synchronous_blend_spaces.clear();
}

CZAnimationBridge::BlendSpaceState &CZAnimationBridge::blend_space(AnimationTree *p_tree, const StringName &p_path) {
	if (evaluation_state && evaluation_state->mixer == p_tree) {
		BlendSpaceState *space = evaluation_state->blend_spaces.getptr(p_path);
		CRASH_COND_MSG(!space, "BlendSpace instance was not prepared before worker evaluation.");
		return *space;
	}
	State **state = mixer_states.getptr(p_tree->get_instance_id());
	return state ? (*state)->blend_spaces[p_path] : synchronous_blend_spaces[p_tree->get_instance_id()][p_path];
}

void CZAnimationBridge::invalidate_graph(AnimationTree *p_tree) {
	synchronize(p_tree);
	State **state = mixer_states.getptr(p_tree->get_instance_id());
	if (state) {
		(*state)->blend_spaces.clear();
	}
	synchronous_blend_spaces.erase(p_tree->get_instance_id());
}

void CZAnimationBridge::interpolate_blend_space_weights(AnimationTree *p_tree, const StringName &p_path, LocalVector<float> &r_weights, float p_speed, double p_delta, bool p_seek) {
	LocalVector<float> &previous = blend_space(p_tree, p_path).weights;
	if (p_speed <= 0) {
		previous.clear();
		return;
	}
	if (previous.size() != r_weights.size() || p_seek) {
		previous = r_weights;
	} else if (p_delta > 0) {
		const float step = p_speed * p_delta;
		for (uint32_t index = 0; index < previous.size(); index++) {
			previous[index] = Math::move_toward(previous[index], r_weights[index], step);
		}
	} else {
		for (uint32_t index = 0; index < previous.size(); index++) {
			r_weights[index] = previous[index];
		}
		return;
	}
	float total = 0;
	for (float weight : previous) {
		total += weight;
	}
	if (total > CMP_EPSILON) {
		for (float &weight : previous) {
			weight /= total;
		}
	} else {
		previous = r_weights;
	}
	for (uint32_t index = 0; index < previous.size(); index++) {
		r_weights[index] = previous[index];
	}
}

double CZAnimationBridge::resolve_blend_space_cycle_duration(const LocalVector<float> &p_weights, const LocalVector<double> &p_lengths, double p_override, double &r_effective) {
	double sum = 0;
	double total = 0;
	for (uint32_t index = 0; index < MIN(p_weights.size(), p_lengths.size()); index++) {
		if (p_weights[index] > 0 && p_lengths[index] > CMP_EPSILON) {
			sum += p_weights[index] * p_lengths[index];
			total += p_weights[index];
		}
	}
	r_effective = total > CMP_EPSILON ? sum / total : 0;
	return p_override > CMP_EPSILON ? p_override : r_effective;
}
