/**************************************************************************/
/*  skeleton_animation_pose.h                                                */
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

#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "scene/3d/copy_transform_modifier_3d.h"
#include "scene/3d/two_bone_ik_3d.h"

// One owner prepares/publishes on the scene thread; exactly one worker evaluates.
class SkeletonAnimationPose : public RefCounted {
	GDCLASS(SkeletonAnimationPose, RefCounted);
	friend class Skeleton3D;
	struct BonePose {
		int parent = -1, offset = 0, span = 0;
		bool enabled = true, modified = false;
		Vector3 skin_scale = Vector3(1, 1, 1);
		Vector3 position, scale = Vector3(1, 1, 1), base_position, base_scale;
		Quaternion rotation, base_rotation;
		Transform3D rest, global_rest, local, global, base, base_global, before_modifier;
	};
	struct Modifier {
		enum Kind { TWO_BONE, COPY, AIM } kind;
		ObjectID id;
		real_t influence = 1;
		int begin = 0, count = 0;
	};
	struct TwoBoneInput {
		TwoBoneIK3D::TwoBoneIK3DSetting settings;
		IKModifier3D::IKModifier3DSolverInfo root, middle;
		bool mutable_axes = true;
		Vector3 target, pole;
		ObjectID target_id, pole_id, modifier_id;
		NodePath target_path, pole_path;
		int setting_index = 0;
	};
	struct CopyInput {
		CopyTransformModifier3D::CopyTransform3DSetting settings;
		Transform3D target;
		ObjectID target_id, owner_id;
		int setting_index = -1;
		NodePath target_path;
	};
	struct AimInput {
		ObjectID modifier_id, grip_target_id, elbow_pole_id;
		int aim = -1, hand = -1, hips = -1, chest = -1, upper_chest = -1;
		int left_leg = -1, right_leg = -1, left_arm = -1, left_elbow = -1;
		real_t hip_weight = 0;
		Transform3D hand_to_muzzle, hand_to_grip, muzzle, grip;
		Vector3 target, up = Vector3(0,1,0), elbow_pole;
		bool has_grip = false;
		real_t error_degrees = 0;
	} aim;
	struct GroundInput {
		bool hit = false;
		Vector3 position, normal;
	};
	struct IKInput {
		bool configured = false, has_hand_target = false;
		Transform3D hand_target_world;
		real_t hand_weight = 0, left_weight = 0, right_weight = 0;
		int hips = -1, left_upper_arm = -1, left_lower_arm = -1;
		int left_foot = -1, right_foot = -1, left_toes = -1, right_toes = -1;
		int left_upper_leg = -1, left_lower_leg = -1, right_upper_leg = -1, right_lower_leg = -1;
		Vector3 arm_rest_pole, left_rest_pole, right_rest_pole;
		real_t left_length = 0, right_length = 0, reach_epsilon = 0;
		real_t maximum_slope = 0, sole_offset = 0, pelvis_drop = 0, pelvis_raise = 0;
		GroundInput left_ground, right_ground;
		ObjectID hand_target, elbow_pole, pelvis_target, left_target, left_pole, right_target, right_pole;
	} ik;
	struct GeneratedTarget {
		ObjectID id;
		Transform3D transform;
		bool position_only = false;
	};
	LocalVector<GeneratedTarget> generated_targets;
	void generate_ik_targets();
	bool create_foot_target(const Transform3D &p_pose, const GroundInput &p_ground, int p_toes, Transform3D &r_target);
	void set_target(ObjectID p_id, const Transform3D &p_target, bool p_position_only = false);
	static Vector3 calculate_pole(const Vector3 &p_root, const Vector3 &p_middle, const Vector3 &p_target, const Vector3 &p_rest);
	struct SkinOutput {
		Ref<Skin> skin;
		RID rendering_skeleton;
		LocalVector<int> indices;
		LocalVector<Transform3D> bind_poses;
		Vector<float> buffers[2];
		int buffer_index = 0;
	};
	LocalVector<SkinOutput> skins;
	uint64_t skin_buffer_copies = 0;
	void update_skin_buffers();
	ObjectID skeleton_id;
	uint64_t skeleton_version = 0;
	bool owns_callback_mode = false;
	Skeleton3D::ModifierCallbackModeProcess saved_callback_mode = Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_IDLE;
	real_t motion_scale = 1;
	bool show_rest = false, evaluated = false, aim_evaluated = false;
	Ref<SkeletonAnimationPose> copy_source;
	int copy_source_bone = -1, copy_target_bone = -1;
	uint64_t binding_version = 0;
	uint64_t input_version = 0;
	struct PoseSample {
		Vector3 position, scale;
		Quaternion rotation;
	};
	LocalVector<PoseSample> samples[2];
	double sample_times[2] = { 0, 0 };
	int sample_index = 0, sample_count = 0;
	double frame_time = 0, interpolation_delay = 0;
	bool sample_requested = true, display_requested = true;
	void store_sample();
	Transform3D world, world_interpolated;
	Ref<SkeletonAnimationPose> attachment_source;
	int attachment_bone = -1;
	Transform3D attachment_offset;
	Transform3D attachment_scale_transform;
	bool attachment_disable_scale = false;
	struct Socket {
		int bone = -1;
		Vector3 offset;
		Transform3D parent_from_skeleton, result;
		Basis marker_basis;
		bool enabled = false;
		bool dirty = true, initialized = false;
		Basis last_metric;
		Transform3D last_bone;
	};
	Socket sockets[2];
	String fallback_reason;
	LocalVector<BonePose> bones;
	LocalVector<int> order;
	LocalVector<bool> dirty;
	LocalVector<Modifier> modifiers;
	LocalVector<TwoBoneInput> two_bone;
	LocalVector<CopyInput> copies;
	LocalVector<int> scratch_path;
	void mark_dirty(int p_bone);
	void update_globals();
	Transform3D chain_rest(int p_bone, int p_root, bool p_mutable);
	void solve_two_bone(TwoBoneInput &r_input);
	void solve_copy(CopyInput &r_input);
	void solve_aim();
	void generate_grip_target();
	void rotate_global(int p_bone, const Quaternion &p_rotation);
	bool fail(const String &p_reason);
protected:
	static void _bind_methods();
public:
	void configure_attachment(const Ref<SkeletonAnimationPose> &p_source, int p_bone, const Transform3D &p_scale_transform, const Transform3D &p_offset, bool p_disable_scale);
	void set_socket_input(int p_index, int p_bone, const Vector3 &p_offset, const Transform3D &p_parent, const Basis &p_marker_basis, bool p_enabled);
	Transform3D get_socket_transform(int p_index) const;
	void evaluate_sockets();
	bool matches_current_inputs() const;
	bool capture(Skeleton3D *p_skeleton);
	bool capture_current();
	bool prepare_frame(bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval);
	bool needs_sample() const { return sample_requested; }
	bool needs_display() const { return display_requested; }
	bool supports_interpolation() const;
	void interpolate_frame();
	void release();
	uint64_t get_skin_buffer_copies() const { return skin_buffer_copies; }
	int get_bone_count() const { return bones.size(); }
	Transform3D get_base_local_pose(int p_bone) const;
	Transform3D get_base_global_pose(int p_bone) const;
	~SkeletonAnimationPose();
	void configure_aim(uint64_t p_modifier_id, const Dictionary &p_input);
	void configure_ik(const Dictionary &p_input);
	void set_aim_input(real_t p_hip_weight, const Transform3D &p_muzzle, bool p_has_grip, const Transform3D &p_grip, const Vector3 &p_target, const Vector3 &p_up);
	void set_ik_hand_input(bool p_has_target, const Transform3D &p_target, const Vector3 &p_weights);
	void set_ik_ground_input(bool p_left_hit, const Vector3 &p_left_position, const Vector3 &p_left_normal, bool p_right_hit, const Vector3 &p_right_position, const Vector3 &p_right_normal);
	void configure_copy_source(const Ref<SkeletonAnimationPose> &p_source, int p_source_bone, int p_target_bone);
	void set_bone_components(int p_bone, const Vector3 &p_position, const Quaternion &p_rotation, const Vector3 &p_scale, int p_mask);
	void evaluate();
	bool publish();
	Transform3D get_global_pose(int p_bone);
	Transform3D get_local_pose(int p_bone) const;
	real_t get_motion_scale() const { return motion_scale; }
	ObjectID get_skeleton_id() const { return skeleton_id; }
	uint64_t get_skeleton_instance_id() const { return uint64_t(skeleton_id); }
	String get_fallback_reason() const { return fallback_reason; }
	Transform3D get_aim_muzzle() const { return aim.muzzle; }
	real_t get_aim_error_degrees() const { return aim.error_degrees; }
	// Adapter consumed by the same templated solvers as Skeleton3D.
	int get_bone_parent(int p_bone) const { return bones[p_bone].parent; }
	Transform3D get_bone_pose(int p_bone) const { return get_local_pose(p_bone); }
	Transform3D get_bone_global_pose(int p_bone) { return get_global_pose(p_bone); }
	Transform3D get_bone_rest(int p_bone) const { return bones[p_bone].rest; }
	Transform3D get_bone_global_rest(int p_bone) const { return bones[p_bone].global_rest; }
	Vector3 get_bone_pose_position(int p_bone) const { return bones[p_bone].position; }
	Quaternion get_bone_pose_rotation(int p_bone) const { return bones[p_bone].rotation; }
	Vector3 get_bone_pose_scale(int p_bone) const { return bones[p_bone].scale; }
	void set_bone_pose_position(int p_bone, const Vector3 &p_position);
	void set_bone_pose_rotation(int p_bone, const Quaternion &p_rotation);
	void set_bone_pose_scale(int p_bone, const Vector3 &p_scale);
	void set_bone_global_pose(int p_bone, const Transform3D &p_pose);
};
