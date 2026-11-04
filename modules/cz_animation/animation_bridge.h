/**************************************************************************/
/*  animation_bridge.h                                                    */
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

#pragma once

#include "look_at_solver.h"

#include "core/object/object_id.h"
#include "core/templates/hash_map.h"
#include "scene/animation/animation_player.h"
#include "scene/animation/animation_tree.h"

class AnimationBatchProcessor;
class SkeletonAnimationPose;
class Skeleton3D;
class LookAtModifier3D;
class SkeletonModifier3D;

// The only project type with access to official animation internals. All mutable
// batch state belongs to an entry, never to an AnimationMixer or AnimationNode.
class CZAnimationBridge {
public:
	struct State;
	struct MethodValidationStatistics {
		uint64_t cache_hits = 0;
		uint64_t scans = 0;
		uint64_t animations = 0;
		uint64_t tracks = 0;
		uint64_t keys = 0;
	};
	struct BlendSpaceState {
		LocalVector<float> weights;
		LocalVector<float> target_weights;
		LocalVector<double> lengths;
	};
	static BlendSpaceState &blend_space(AnimationTree *p_tree, const StringName &p_path);
	static void interpolate_blend_space_weights(AnimationTree *p_tree, const StringName &p_path, LocalVector<float> &r_weights, float p_speed, double p_delta, bool p_seek);
	static double resolve_blend_space_cycle_duration(const LocalVector<float> &p_weights, const LocalVector<double> &p_lengths, double p_override, double &r_effective);
	static void invalidate_graph(AnimationTree *p_tree);
	static State *create(AnimationMixer *p_mixer, AnimationBatchProcessor *p_owner, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, State *p_parent);
	static void destroy(State *p_state);
	static void detach_owner(State *p_state);
	static void reset_method_validation_statistics(State *p_state);
	static MethodValidationStatistics get_method_validation_statistics(const State *p_state);
	static bool prepare(State *p_state, double p_delta, const LocalVector<Ref<SkeletonAnimationPose>> &p_poses);
	static void evaluate(State *p_state);
	static bool publish(State *p_state);
	static void discard(State *p_state);
	static bool succeeded(const State *p_state);
	static String fallback(const State *p_state);
	static AnimationMixer *mixer(State *p_state);
	static void synchronize(AnimationMixer *p_mixer = nullptr);
	static void synchronize_resource();
	static void set_callback_mode(AnimationMixer *p_mixer, AnimationMixer::AnimationCallbackModeProcess p_mode);
	static void begin_player_mutation(AnimationMixer *p_mixer);
	static void end_player_mutation();
	static bool is_evaluating(const AnimationMixer *p_mixer);
	static bool is_processing_pose(const SkeletonAnimationPose *p_pose);
	static void complete_pose(const SkeletonAnimationPose *p_pose);
	static bool skip_track(const AnimationMixer *p_mixer, Animation::TrackType p_type, bool p_root_motion);
	static bool skip_apply(const AnimationMixer *p_mixer, Animation::TrackType p_type, bool p_root_motion);
	static bool queue_animation_signal(AnimationMixer *p_mixer, const StringName &p_signal, const StringName &p_animation);
	static bool queue_resource_signal(const Ref<Resource> &p_resource, const StringName &p_signal, const StringName &p_value);
	static bool set_parameter_group(AnimationTree *p_tree, const TypedArray<StringName> &p_names, const Variant &p_value);
	static uint64_t skeleton_version(const Skeleton3D *p_skeleton);
	static uint64_t skeleton_input_version(const Skeleton3D *p_skeleton);
	static void observe_skeleton(Skeleton3D *p_skeleton);
	static void unobserve_skeleton(ObjectID p_id);
	static void observe_skeleton_layout(Skeleton3D *p_skeleton);
	static void unobserve_skeleton_layout(ObjectID p_id);
	static void acquire_skeleton_control(Skeleton3D *p_skeleton);
	static void release_skeleton_control(ObjectID p_id);
	static void invalidate_skeleton(Skeleton3D *p_skeleton, bool p_binding = true);
	static void invalidate_binding_cache(AnimationMixer *p_mixer);
	static void forget_object(ObjectID p_id);
	static void set_modifiers_enabled(Skeleton3D *p_skeleton, bool p_enabled);
	static bool modifiers_enabled(const Skeleton3D *p_skeleton);
	static bool modifier_applies_influence(const SkeletonModifier3D *p_modifier);
	static bool capture_skeleton(SkeletonAnimationPose *p_pose, Skeleton3D *p_skeleton, bool p_animated);
	static bool publish_skeleton(SkeletonAnimationPose *p_pose);
	static CZLookAtSolver capture_look_at(const LookAtModifier3D *p_modifier);
	static void publish_look_at(LookAtModifier3D *p_modifier, const CZLookAtSolver &p_solver);
	static bool process_look_at(LookAtModifier3D *p_modifier, Skeleton3D *p_skeleton);
	static void shutdown();
};
