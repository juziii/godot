/**************************************************************************/
/*  bone_rotation_offset_modifier_3d.h                                    */
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
/* Software), to deal in the Software without restriction, including      */
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

#include "scene/3d/skeleton_modifier_3d.h"

class BoneRotationOffsetModifier3D : public SkeletonModifier3D {
	GDCLASS(BoneRotationOffsetModifier3D, SkeletonModifier3D);

public:
	enum RotationSpace {
		ROTATION_SPACE_LOCAL,
		ROTATION_SPACE_SKELETON,
	};

private:
	PackedStringArray bone_names;
	PackedFloat32Array rotation_weights;
	Vector<Quaternion> rotation_offsets;
	Vector<Quaternion> rotation_offset_blend_from;
	Vector<Quaternion> rotation_offset_blend_to;
	float rotation_offset_blend_weight = 0.0f;
	bool has_rotation_offset_blend_samples = false;
	Quaternion rotation;
	RotationSpace rotation_space = ROTATION_SPACE_LOCAL;
	bool use_initial_global_pose = false;
	struct Target {
		int bone = -1;
		int name_index = -1;
		int depth = 0;
	};
	Vector<Target> targets;
	Vector<Quaternion> initial_global_rotations;
	bool targets_dirty = true;
	ObjectID targets_skeleton_id;
	uint64_t targets_skeleton_version = 0;
	uint64_t targets_binding_version = 0;
	int targets_bone_count = 0;
	void _update_targets(Skeleton3D *p_skeleton);
	const Vector<Target> &_get_targets(Skeleton3D *p_skeleton);
	void _clear_rotation_offset_blend_samples();

	friend class SkeletonAnimationPose;

protected:
	static void _bind_methods();
	void _process_modification(double p_delta) override;

public:
	void set_bone_names(const PackedStringArray &p_bone_names);
	PackedStringArray get_bone_names() const;
	void set_rotation_weights(const PackedFloat32Array &p_weights);
	PackedFloat32Array get_rotation_weights() const;
	void set_rotation_offsets(const TypedArray<Quaternion> &p_offsets);
	void set_rotation_offsets_packed(const PackedVector4Array &p_offsets);
	bool set_rotation_offset_blend_samples_packed(const PackedVector4Array &p_from, const PackedVector4Array &p_to);
	void set_rotation_offset_blend_weight(float p_weight);
	float get_rotation_offset_blend_weight() const;
	TypedArray<Quaternion> get_rotation_offsets() const;
	void set_bone_rotation_offset(int p_index, const Quaternion &p_rotation);
	Quaternion get_bone_rotation_offset(int p_index) const;
	void clear_rotation_offsets();
	void set_rotation_offset(const Quaternion &p_rotation);
	Quaternion get_rotation_offset() const;
	void set_rotation_space(RotationSpace p_space);
	RotationSpace get_rotation_space() const;
	void set_use_initial_global_pose(bool p_enabled);
	bool is_using_initial_global_pose() const;

	// Shared by the synchronous Skeleton3D path and detached pose evaluation.
	template <typename Pose>
	static void apply(Pose *p_pose, int p_bone, const Quaternion &p_rotation, RotationSpace p_space, float p_weight) {
		if (p_weight <= 0.0f) {
			return;
		}
		const Quaternion offset = Quaternion().slerp(p_rotation, p_weight);
		if (p_space == ROTATION_SPACE_LOCAL) {
			p_pose->set_bone_pose_rotation(p_bone, p_pose->get_bone_pose_rotation(p_bone) * offset);
			return;
		}
		const int parent = p_pose->get_bone_parent(p_bone);
		Transform3D global_pose = p_pose->get_bone_global_pose(p_bone);
		global_pose.basis = Basis(offset * global_pose.basis.get_rotation_quaternion()).scaled(global_pose.basis.get_scale());
		const Transform3D local_pose = parent >= 0 ? p_pose->get_bone_global_pose(parent).affine_inverse() * global_pose : global_pose;
		p_pose->set_bone_pose_rotation(p_bone, local_pose.basis.get_rotation_quaternion());
	}
	// Applies a target global rotation derived from the pose at modifier entry.
	// Parent targets must be applied first so each child can convert through the
	// updated parent without accumulating the parent's offset a second time.
	template <typename Pose>
	static void apply_from_initial_global_pose(Pose *p_pose, int p_bone, const Quaternion &p_rotation, float p_weight, const Quaternion &p_initial_global_rotation) {
		if (p_weight <= 0.0f) {
			return;
		}
		const Quaternion offset = Quaternion().slerp(p_rotation, p_weight);
		Transform3D global_pose = p_pose->get_bone_global_pose(p_bone);
		global_pose.basis = Basis(offset * p_initial_global_rotation).scaled(global_pose.basis.get_scale());
		const int parent = p_pose->get_bone_parent(p_bone);
		const Transform3D local_pose = parent >= 0 ? p_pose->get_bone_global_pose(parent).affine_inverse() * global_pose : global_pose;
		p_pose->set_bone_pose_rotation(p_bone, local_pose.basis.get_rotation_quaternion());
	}
};

VARIANT_ENUM_CAST(BoneRotationOffsetModifier3D::RotationSpace);
