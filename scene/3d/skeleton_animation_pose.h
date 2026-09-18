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
#include "scene/3d/pawn_animation_pose.h"
#include "scene/3d/two_bone_ik_3d.h"

// One owner prepares/publishes on the scene thread; exactly one worker evaluates.
class SkeletonAnimationPose : public RefCounted {
	GDCLASS(SkeletonAnimationPose, RefCounted);
	friend class Skeleton3D;
	friend class PawnAnimationPose;

public:
	// Observability of the last publish round, used to profile Animation.Publish.
	struct PublishStats {
		uint32_t skins = 0; // skeleton_set_buffer calls issued.
		uint64_t skin_bytes = 0; // Bytes handed to the rendering server.
		uint32_t attachments = 0; // BoneAttachment3D nodes driven by this skeleton.
		uint32_t pose_updated_observers = 0; // 1 when pose_updated has connections.
		uint32_t skeleton_updated_observers = 0; // 1 when skeleton_updated has connections.
		uint32_t target_writes = 0; // Node3D transforms written for aim/grip/targets.
		uint32_t modifier_signals = 0; // modification_processed emissions.
		bool fast_path = false; // Published through the native-attachment fast path.
	};

private:
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
	PawnAnimationPose pawn_pose{ *this };
	struct GeneratedTarget {
		ObjectID id;
		Transform3D transform;
		bool position_only = false;
	};
	LocalVector<GeneratedTarget> generated_targets;
	void set_target(ObjectID p_id, const Transform3D &p_target, bool p_position_only = false);
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
	// Filled by Skeleton3D (friend) while publishing; reset by AnimationMixer before
	// each publish round. Main thread only.
	PublishStats publish_stats;
	ObjectID skeleton_id;
	uint64_t skeleton_version = 0;
	bool owns_callback_mode = false;
	Skeleton3D::ModifierCallbackModeProcess saved_callback_mode = Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_IDLE;
	real_t motion_scale = 1;
	bool show_rest = false, evaluated = false;
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
	LocalVector<Socket> sockets;
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
	const PublishStats &get_publish_stats() const { return publish_stats; }
	void reset_publish_stats() { publish_stats = PublishStats(); }
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
	Transform3D get_aim_muzzle() const { return pawn_pose.get_aim_muzzle(); }
	real_t get_aim_error_degrees() const { return pawn_pose.get_aim_error_degrees(); }
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
