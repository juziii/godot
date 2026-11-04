/**************************************************************************/
/*  skeleton_animation_pose.h                                             */
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

#include "bone_lod.h"
#include "bone_rotation_offset_modifier_3d.h"
#include "look_at_solver.h"
#include "modifier_solvers.h"
#include "pawn_pose_solver.h"

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "scene/3d/skeleton_3d.h"

class CZAnimationBridge;

class SkeletonAnimationPose : public RefCounted {
	GDCLASS(SkeletonAnimationPose, RefCounted);
	friend class CZAnimationBridge;
	friend class AnimationBatchProcessor;
	friend class CZPawnPose;

	struct Bone {
		int parent = -1;
		Transform3D rest;
		Transform3D global_rest;
		Vector3 skin_scale = Vector3(1, 1, 1);
		bool enabled = true;
	};
	struct SkinOutput {
		Ref<Skin> skin;
		RID rid;
		LocalVector<int> bones;
		LocalVector<Transform3D> bind_poses;
		Vector<float> buffer;
	};
	struct Socket {
		int bone = -1;
		Vector3 offset;
		Transform3D parent;
		Basis marker;
		Transform3D result;
		bool enabled = false;
	};
	struct CopyInput {
		CopyTransformModifier3D::CopyTransform3DSetting settings;
		Transform3D target;
		ObjectID target_id;
		NodePath target_path;
	};
	struct TwoBoneInput {
		TwoBoneIK3D::TwoBoneIK3DSetting settings;
		IKModifier3D::IKModifier3DSolverInfo root;
		IKModifier3D::IKModifier3DSolverInfo middle;
		ObjectID target_id;
		ObjectID pole_id;
		NodePath target_path;
		NodePath pole_path;
		Vector3 target;
		Vector3 pole;
		int setting_index = -1;
		bool mutable_axes = true;
	};
	struct Modifier {
		ObjectID id;
		real_t influence = 1;
		bool aim = false;
		bool look_at = false;
		bool copy = false;
		bool two_bone = false;
		bool initial_global = false;
		BoneRotationOffsetModifier3D::RotationSpace space = BoneRotationOffsetModifier3D::ROTATION_SPACE_LOCAL;
		LocalVector<int> targets;
		LocalVector<Quaternion> rotations;
		LocalVector<real_t> weights;
		LocalVector<CopyInput> copies;
		LocalVector<TwoBoneInput> two_bones;
		CZLookAtSolver look_solver;
		int look_bone = -1;
		int origin_bone = -1;
		int origin_from = 0;
		Vector3 origin_offset;
		Vector3 target;
		Transform3D external_origin;
		bool has_target = false;
		bool has_external_origin = false;
		ObjectID look_target_id;
		ObjectID origin_target_id;
		NodePath look_target_path;
		NodePath origin_target_path;
	};
	struct GeneratedTarget {
		ObjectID id;
		Transform3D transform;
		bool position_only = false;
	};
	LocalVector<GeneratedTarget> generated_targets;
	LocalVector<Modifier> modifiers;
	LocalVector<Quaternion> initial_global_rotations;
	LocalVector<int> scratch_path;
	CZPawnPose pawn_pose{ *this };
	ObjectID skeleton_id;
	uint64_t binding_version = 0;
	uint64_t input_version = 0;
	uint64_t structural_version = 0;
	uint64_t layout_version = 0;
	uint64_t capture_generation = 0;
	LocalVector<Bone> bones;
	LocalVector<int> parents;
	LocalVector<Transform3D> rest_transforms;
	LocalVector<int> first_disabled;
	LocalVector<int> required;
	LocalVector<int> required_next;
	LocalVector<int> configured_required;
	HashMap<int, int> external_requirements;
	LocalVector<SkinOutput> skins;
	LocalVector<Socket> sockets;
	CZBoneLod lod;
	PackedStringArray lod_names;
	PackedInt32Array lod_levels;
	int requested_lod = 0;
	int bound_lod = -1;
	bool force_full_bones = false;
	bool bound_force_full_bones = false;
	bool binding_dirty = true;
	bool reference_capture_dirty = true;
	bool evaluated = false;
	bool evaluating_pose = false;
	bool display_valid = false;
	bool sample_requested = true;
	bool display_requested = true;
	bool simd_enabled = true;
	static bool global_simd_enabled;
	static bool global_bone_lod_enabled;
	uint64_t skin_buffer_copies = 0;
	uint64_t sample_blend_us = 0;
	uint64_t interpolate_us = 0;
	uint64_t skin_pack_us = 0;
	uint64_t matrix_us = 0;
	uint64_t captured_bones = 0;
	bool owns_callback = false;
	bool observed = false;
	struct Cleanup {
		ObjectID skeleton;
		bool owns_callback = false;
		bool observed = false;
		Ref<SkeletonAnimationPose> copy_source;
		int copy_source_bone = -1;
		Ref<SkeletonAnimationPose> attachment_source;
		int attachment_bone = -1;
	};
	static Mutex cleanup_mutex;
	static LocalVector<Cleanup *> deferred_cleanup;
	real_t motion_scale = 1;
	bool show_rest = false;
	String fallback_reason;
	Transform3D world;
	Transform3D world_interpolated;
	CZPoseSoA reference_pose;
	LocalVector<uint8_t> reference_mask;
	CZPoseSoA sampled;
	CZPoseSoA blended;
	CZPoseSoA base_pose;
	CZPoseSoA displayed_base;
	CZPoseSoA retained_base;
	CZPoseSoA before_modifier;
	CZPoseSoA displayed;
	CZPoseSoA history[2];
	CZPoseSoA base_history[2];
	LocalVector<Transform3D> globals;
	LocalVector<Transform3D> base_globals;
	double sample_times[2] = { 0, 0 };
	double frame_time = 0;
	double interpolation_delay = 0;
	int history_index = 0;
	int history_count = 0;
	Ref<SkeletonAnimationPose> copy_source;
	int copy_source_bone = -1;
	int copy_target_bone = -1;
	Ref<SkeletonAnimationPose> attachment_source;
	int attachment_bone = -1;
	Transform3D attachment_scale;
	Transform3D attachment_offset;
	bool attachment_disable_scale = false;
	Dictionary aim_configuration;
	Dictionary ik_configuration;
	uint64_t aim_modifier_id = 0;
	void _update_globals();
	bool _capture(Skeleton3D *p_skeleton, bool p_animated);
	bool _fail(const String &p_reason);
	void _pack_skins();
	void set_target(ObjectID p_id, const Transform3D &p_target, bool p_position_only = false);
	void _refresh_configured_requirements();
	void _require_external(int p_bone, int p_delta);
	void _release_capture();
	bool _depends_on(const SkeletonAnimationPose *p_pose) const;
	void _solve_copy(CopyInput &r_input);
	void _solve_two_bone(TwoBoneInput &r_input);
	Transform3D _chain_rest(int p_bone, int p_root, bool p_mutable);
	const CZPoseSoA &_current_pose() const { return !evaluating_pose && display_valid ? displayed : blended; }

protected:
	static void _bind_methods();

public:
	// Bound inputs/publication and queries are scene-thread APIs. Queries join
	// their registered producer without publishing events. Detached evaluate()
	// may run on one worker exclusively owned and joined by its caller; internal
	// solver getters in that evaluation read only already ordered pose sources.
	bool capture(Skeleton3D *p_skeleton);
	bool capture_current();
	bool matches_current_inputs() const;
	bool prepare_frame(bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval, bool p_force_full_bones = false);
	void configure_bone_lod(const PackedStringArray &p_bone_names, const PackedInt32Array &p_first_disabled_lod);
	void set_bone_lod(int p_level);
	int get_bone_lod() const { return requested_lod; }
	int get_active_bone_count() const { return lod.get_active_bone_count(); }
	int get_bone_count() const { return bones.size(); }
	bool needs_sample() const { return sample_requested; }
	bool needs_display() const { return display_requested; }
	bool supports_interpolation() const;
	void evaluate();
	void interpolate_frame();
	bool publish();
	void release();
	static void drain_deferred_cleanup();
	String get_fallback_reason() const { return fallback_reason; }
	ObjectID get_skeleton_id() const { return skeleton_id; }
	uint64_t get_skeleton_instance_id() const { return uint64_t(skeleton_id); }
	real_t get_motion_scale() const { return motion_scale; }
	uint64_t get_skin_buffer_copies() const;
	uint64_t get_storage_bytes() const;
	static void set_simd_enabled(bool p_enabled);
	static bool is_simd_enabled() { return global_simd_enabled; }
	static void set_bone_lod_enabled(bool p_enabled);
	static bool is_bone_lod_enabled() { return global_bone_lod_enabled; }
	Transform3D get_global_pose(int p_bone);
	Transform3D get_local_pose(int p_bone) const;
	Transform3D get_base_local_pose(int p_bone) const;
	Transform3D get_base_global_pose(int p_bone) const;
	void set_bone_components(int p_bone, const Vector3 &p_position, const Quaternion &p_rotation, const Vector3 &p_scale, int p_mask);
	int get_bone_parent(int p_bone) const { return bones[p_bone].parent; }
	Transform3D get_bone_global_pose(int p_bone) { return get_global_pose(p_bone); }
	Transform3D get_bone_pose(int p_bone) const { return get_local_pose(p_bone); }
	Transform3D get_bone_rest(int p_bone) const { return bones[p_bone].rest; }
	Transform3D get_bone_global_rest(int p_bone) const { return bones[p_bone].global_rest; }
	Quaternion get_bone_pose_rotation(int p_bone) const;
	Vector3 get_bone_pose_position(int p_bone) const;
	Vector3 get_bone_pose_scale(int p_bone) const;
	void set_bone_pose_position(int p_bone, const Vector3 &p_position);
	void set_bone_pose_scale(int p_bone, const Vector3 &p_scale);
	void set_bone_pose_rotation(int p_bone, const Quaternion &p_rotation);
	void set_bone_global_pose(int p_bone, const Transform3D &p_transform);
	void configure_copy_source(const Ref<SkeletonAnimationPose> &p_source, int p_source_bone, int p_target_bone);
	void configure_attachment(const Ref<SkeletonAnimationPose> &p_source, int p_bone, const Transform3D &p_scale_transform, const Transform3D &p_offset, bool p_disable_scale);
	void set_socket_input(int p_index, int p_bone, const Vector3 &p_offset, const Transform3D &p_parent, const Basis &p_marker_basis, bool p_enabled);
	Transform3D get_socket_transform(int p_index) const;
	void evaluate_sockets();
	void configure_aim(uint64_t p_modifier_id, const Dictionary &p_input);
	void configure_ik(const Dictionary &p_input);
	void set_aim_input(real_t p_hip_weight, const Transform3D &p_muzzle, bool p_has_grip, const Transform3D &p_grip, const Vector3 &p_target, const Vector3 &p_up);
	void set_ik_hand_input(bool p_has_target, const Transform3D &p_target, const Vector3 &p_weights);
	void set_ik_ground_input(bool p_left_hit, const Vector3 &p_left_position, const Vector3 &p_left_normal, bool p_right_hit, const Vector3 &p_right_position, const Vector3 &p_right_normal);
	void set_ik_ground_motion_input(bool p_left_hit, const Vector3 &p_left_position, const Vector3 &p_left_normal, const Vector3 &p_left_smoothed_normal, bool p_right_hit, const Vector3 &p_right_position, const Vector3 &p_right_normal, const Vector3 &p_right_smoothed_normal);
	Transform3D get_aim_muzzle() const;
	real_t get_aim_error_degrees() const;
	~SkeletonAnimationPose();
};
