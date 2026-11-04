/**************************************************************************/
/*  modifier_solvers.h                                                    */
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

#include "scene/3d/copy_transform_modifier_3d.h"
#include "scene/3d/two_bone_ik_3d.h"

namespace CZModifierSolvers {
using CopyTransform3DSetting = CopyTransformModifier3D::CopyTransform3DSetting;
using AxisFlag = CopyTransformModifier3D::AxisFlag;
static constexpr AxisFlag AXIS_FLAG_X = CopyTransformModifier3D::AXIS_FLAG_X;
static constexpr AxisFlag AXIS_FLAG_Y = CopyTransformModifier3D::AXIS_FLAG_Y;
static constexpr AxisFlag AXIS_FLAG_Z = CopyTransformModifier3D::AXIS_FLAG_Z;
static constexpr AxisFlag AXIS_FLAG_ALL = CopyTransformModifier3D::AXIS_FLAG_ALL;
static constexpr auto TRANSFORM_FLAG_POSITION = CopyTransformModifier3D::TRANSFORM_FLAG_POSITION;
static constexpr auto TRANSFORM_FLAG_ROTATION = CopyTransformModifier3D::TRANSFORM_FLAG_ROTATION;
static constexpr auto TRANSFORM_FLAG_SCALE = CopyTransformModifier3D::TRANSFORM_FLAG_SCALE;
template <typename Pose>
static void copy_pose(Pose *p_skeleton, CopyTransform3DSetting *setting, int p_apply_bone, const Transform3D &p_destination, float p_amount) {
	Transform3D destination = p_destination;
	Vector3 dest_pos = destination.origin;
	Quaternion dest_rot = destination.basis.get_rotation_quaternion();
	Vector3 dest_scl = destination.basis.get_scale();

	// Mask pos and scale.
	for (int i = 0; i < 3; i++) {
		if (!setting->axis_flags.has_flag(static_cast<AxisFlag>(1 << i))) {
			dest_pos[i] = 0.0;
			dest_scl[i] = 1.0;
		}
	}

	// Mask rot.
	switch (static_cast<int>(setting->axis_flags)) {
		case 0: {
			dest_rot = Quaternion();
		} break;
		case AXIS_FLAG_X: {
			Vector3 axis = SkeletonModifier3D::get_vector_from_axis(Vector3::AXIS_X);
			dest_rot = Quaternion(axis, SkeletonModifier3D::get_roll_angle(dest_rot, axis));
		} break;
		case AXIS_FLAG_Y: {
			Vector3 axis = SkeletonModifier3D::get_vector_from_axis(Vector3::AXIS_Y);
			dest_rot = Quaternion(axis, SkeletonModifier3D::get_roll_angle(dest_rot, axis));
		} break;
		case AXIS_FLAG_Z: {
			Vector3 axis = SkeletonModifier3D::get_vector_from_axis(Vector3::AXIS_Z);
			dest_rot = Quaternion(axis, SkeletonModifier3D::get_roll_angle(dest_rot, axis));
		} break;
		case AXIS_FLAG_X | AXIS_FLAG_Y: {
			Vector3 axis = SkeletonModifier3D::get_vector_from_axis(Vector3::AXIS_Z);
			dest_rot = dest_rot * Quaternion(axis, SkeletonModifier3D::get_roll_angle(dest_rot, axis)).inverse();
		} break;
		case AXIS_FLAG_Y | AXIS_FLAG_Z: {
			Vector3 axis = SkeletonModifier3D::get_vector_from_axis(Vector3::AXIS_X);
			dest_rot = dest_rot * Quaternion(axis, SkeletonModifier3D::get_roll_angle(dest_rot, axis)).inverse();
		} break;
		case AXIS_FLAG_Z | AXIS_FLAG_X: {
			Vector3 axis = SkeletonModifier3D::get_vector_from_axis(Vector3::AXIS_Y);
			dest_rot = dest_rot * Quaternion(axis, SkeletonModifier3D::get_roll_angle(dest_rot, axis)).inverse();
		} break;
		case AXIS_FLAG_ALL: {
		} break;
	}

	// Process inversion.
	for (int i = 0; i < 3; i++) {
		AxisFlag axis = static_cast<AxisFlag>(1 << i);
		if (setting->axis_flags.has_flag(axis) && setting->invert_flags.has_flag(axis)) {
			dest_pos[i] *= -1;
			dest_rot[i] *= -1;
			dest_scl[i] = 1.0 / dest_scl[i];
		}
	}
	dest_rot.normalize();

	if (setting->additive) {
		destination.origin = p_skeleton->get_bone_pose_position(p_apply_bone) + dest_pos;
		destination.basis = p_skeleton->get_bone_pose_rotation(p_apply_bone) * Basis(dest_rot);
		destination.basis.scale_local(p_skeleton->get_bone_pose_scale(p_apply_bone) * dest_scl);
	} else if (setting->is_relative()) {
		Transform3D rest = p_skeleton->get_bone_rest(p_apply_bone);
		destination.origin = rest.origin + dest_pos;
		destination.basis = rest.basis.get_rotation_quaternion() * Basis(dest_rot);
		destination.basis.scale_local(rest.basis.get_scale() * dest_scl);
	} else {
		destination.origin = dest_pos;
		destination.basis = Basis(dest_rot);
		destination.basis.scale_local(dest_scl);
	}

	// Process interpolation depends on the amount.
	destination = p_skeleton->get_bone_pose(p_apply_bone).interpolate_with(destination, p_amount);

	// Apply transform depends on the element mask.
	if (setting->copy_flags.has_flag(TRANSFORM_FLAG_POSITION)) {
		p_skeleton->set_bone_pose_position(p_apply_bone, destination.origin);
	}
	if (setting->copy_flags.has_flag(TRANSFORM_FLAG_ROTATION)) {
		p_skeleton->set_bone_pose_rotation(p_apply_bone, destination.basis.get_rotation_quaternion());
	}
	if (setting->copy_flags.has_flag(TRANSFORM_FLAG_SCALE)) {
		p_skeleton->set_bone_pose_scale(p_apply_bone, destination.basis.get_scale());
	}
}

template <typename Pose>
static void cache_two_bone_vectors(Pose *p_skeleton, TwoBoneIK3D::TwoBoneIK3DSetting *p_setting) {
	if (!p_setting->is_valid()) {
		return;
	}
	p_setting->root_joint_solver_info->current_vector = (p_setting->mid_pos - p_setting->root_pos).normalized();
	p_setting->mid_joint_solver_info->current_vector = (p_setting->end_pos - p_setting->mid_pos).normalized();
}

template <typename Pose>
static void init_two_bone_rotations(Pose *p_skeleton, TwoBoneIK3D::TwoBoneIK3DSetting *p_setting) {
	if (!p_setting->is_valid()) {
		return;
	}

	Quaternion parent_gpose;
	int parent = p_skeleton->get_bone_parent(p_setting->root_bone.bone);
	if (parent >= 0) {
		parent_gpose = p_skeleton->get_bone_global_pose(parent).basis.get_rotation_quaternion();
	}
	p_setting->root_joint_solver_info->current_lrest = p_skeleton->get_bone_pose(p_setting->root_bone.bone).basis.get_rotation_quaternion();
	p_setting->root_joint_solver_info->current_grest = parent_gpose * p_setting->root_joint_solver_info->current_lrest;
	p_setting->root_joint_solver_info->current_grest.normalize();
	p_setting->root_joint_solver_info->current_lpose = p_skeleton->get_bone_pose(p_setting->root_bone.bone).basis.get_rotation_quaternion();
	p_setting->root_joint_solver_info->current_gpose = parent_gpose * p_setting->root_joint_solver_info->current_lpose;
	p_setting->root_joint_solver_info->current_gpose.normalize();
	parent_gpose = p_setting->root_joint_solver_info->current_gpose;

	// Mid joint pose is relative to the root joint pose.
	p_setting->mid_joint_solver_info->current_lrest = p_skeleton->get_bone_global_pose(p_setting->root_bone.bone).basis.get_rotation_quaternion().inverse() * p_skeleton->get_bone_global_pose(p_setting->middle_bone.bone).basis.get_rotation_quaternion();
	p_setting->mid_joint_solver_info->current_grest = parent_gpose * p_setting->mid_joint_solver_info->current_lrest;
	p_setting->mid_joint_solver_info->current_grest.normalize();
	p_setting->mid_joint_solver_info->current_lpose = p_skeleton->get_bone_global_pose(p_setting->root_bone.bone).basis.get_rotation_quaternion().inverse() * p_skeleton->get_bone_global_pose(p_setting->middle_bone.bone).basis.get_rotation_quaternion();
	p_setting->mid_joint_solver_info->current_gpose = parent_gpose * p_setting->mid_joint_solver_info->current_lpose;
	p_setting->mid_joint_solver_info->current_gpose.normalize();

	cache_two_bone_vectors(p_skeleton, p_setting);
}

template <typename Pose>
static void cache_two_bone_rotations(Pose *p_skeleton, TwoBoneIK3D::TwoBoneIK3DSetting *p_setting, const Vector3 &p_pole_destination) {
	if (!p_setting->is_valid()) {
		return;
	}

	Quaternion parent_gpose;
	int parent = p_skeleton->get_bone_parent(p_setting->root_bone.bone);
	if (parent >= 0) {
		parent_gpose = p_skeleton->get_bone_global_pose(parent).basis.get_rotation_quaternion();
	}

	p_setting->root_joint_solver_info->current_lrest = p_skeleton->get_bone_pose(p_setting->root_bone.bone).basis.get_rotation_quaternion();
	p_setting->root_joint_solver_info->current_grest = parent_gpose * p_setting->root_joint_solver_info->current_lrest;
	p_setting->root_joint_solver_info->current_grest.normalize();

	Vector3 from = p_setting->root_joint_solver_info->forward_vector;
	Vector3 to = p_setting->root_joint_solver_info->current_grest.xform_inv(p_setting->root_joint_solver_info->current_vector).normalized();
	p_setting->root_joint_solver_info->current_lpose = p_setting->root_joint_solver_info->current_lrest * SkeletonModifier3D::get_swing(Quaternion(from, to), from);

	p_setting->root_joint_solver_info->current_gpose = parent_gpose * p_setting->root_joint_solver_info->current_lpose;
	p_setting->root_joint_solver_info->current_gpose.normalize();
	Quaternion root_gpose = p_setting->root_joint_solver_info->current_gpose;

	// Mid joint pose is relative to the root joint pose for the case root-mid or mid-end have more than 1 joints.
	p_setting->mid_joint_solver_info->current_lrest = p_skeleton->get_bone_global_pose(p_setting->root_bone.bone).basis.get_rotation_quaternion().inverse() * p_skeleton->get_bone_global_pose(p_setting->middle_bone.bone).basis.get_rotation_quaternion();
	p_setting->mid_joint_solver_info->current_grest = root_gpose * p_setting->mid_joint_solver_info->current_lrest;
	p_setting->mid_joint_solver_info->current_grest.normalize();

	from = p_setting->mid_joint_solver_info->forward_vector;
	to = p_setting->mid_joint_solver_info->current_grest.xform_inv(p_setting->mid_joint_solver_info->current_vector).normalized();
	p_setting->mid_joint_solver_info->current_lpose = p_setting->mid_joint_solver_info->current_lrest * SkeletonModifier3D::get_swing(Quaternion(from, to), from);

	p_setting->mid_joint_solver_info->current_gpose = root_gpose * p_setting->mid_joint_solver_info->current_lpose;
	p_setting->mid_joint_solver_info->current_gpose.normalize();

	bool is_pole_defined = p_setting->pole_direction != SkeletonModifier3D::SECONDARY_DIRECTION_NONE && (p_setting->pole_direction != SkeletonModifier3D::SECONDARY_DIRECTION_CUSTOM || !p_setting->pole_direction_vector.is_zero_approx());
	// Fix roll to align pole vector to plane.
	if (is_pole_defined) {
		// Calc roll angles.
		Quaternion root_roll_rot = Quaternion();
		Quaternion mid_roll_rot = Quaternion();

		// Make roll to align pole_vector onto plane with selecting the point nearer pole_destination.
		Vector3 pole_dir = SkeletonModifier3D::get_projected_normal(p_setting->root_pos, p_setting->end_pos, p_pole_destination);
		if (pole_dir.is_zero_approx()) {
			return;
		}
		Vector3 a = p_setting->mid_joint_solver_info->current_vector.normalized(); // Global roll axis (mid forward in current pose).
		Vector3 k = p_setting->mid_joint_solver_info->current_gpose.xform(p_setting->get_pole_direction_vector()).normalized(); // Global pole vector.
		Vector3 n = pole_dir.cross((p_setting->mid_pos - p_setting->root_pos).normalized()).normalized(); // Global plane normal.

		// Guard: degenerate cases (zero or already parallel)
		if (a.is_zero_approx() || k.is_zero_approx() || n.is_zero_approx() || Math::is_zero_approx(n.dot(k))) {
			return;
		}
		// c0 cosθ + c1 sinθ + c2 = 0
		double c0 = n.dot(k - a * k.dot(a)); // n·(k⊥a)
		double c1 = n.dot(a.cross(k)); // n·(a×k)
		double c2 = n.dot(a) * k.dot(a); // (n·a)(k·a)
		double r = Math::sqrt(c0 * c0 + c1 * c1);
		double cos_arg = CLAMP(-c2 / r, -1.0, 1.0);
		double phi = Math::atan2(c1, c0);
		double acosv = Math::acos(cos_arg);

		// Two candidate angles.
		double t1 = phi + acosv;
		double t2 = phi - acosv;
		Quaternion q1(a, t1);
		Quaternion q2(a, t2);

		// Choose the one whose projected pole points closer to pole side.
		Vector3 pole_proj = SkeletonModifier3D::snap_vector_to_plane(n, pole_dir).normalized();
		Vector3 k1p = SkeletonModifier3D::snap_vector_to_plane(n, q1.xform(k)).normalized();
		Vector3 k2p = SkeletonModifier3D::snap_vector_to_plane(n, q2.xform(k)).normalized();
		double s1 = pole_proj.is_zero_approx() ? Math::abs(t1) : k1p.dot(pole_proj);
		double s2 = pole_proj.is_zero_approx() ? Math::abs(t2) : k2p.dot(pole_proj);

		double t = s1 >= s2 ? t1 : t2;
		root_roll_rot = Quaternion(p_setting->root_joint_solver_info->forward_vector, t);
		mid_roll_rot = Quaternion(p_setting->mid_joint_solver_info->forward_vector, t);

		p_setting->root_joint_solver_info->current_lpose = p_setting->root_joint_solver_info->current_lpose * root_roll_rot;
		p_setting->root_joint_solver_info->current_gpose = parent_gpose * p_setting->root_joint_solver_info->current_lpose;
		p_setting->root_joint_solver_info->current_gpose.normalize();
		root_gpose = p_setting->root_joint_solver_info->current_gpose;

		p_setting->mid_joint_solver_info->current_lpose = root_roll_rot.inverse() * p_setting->mid_joint_solver_info->current_lpose * mid_roll_rot;
		p_setting->mid_joint_solver_info->current_gpose = root_gpose * p_setting->mid_joint_solver_info->current_lpose;
		p_setting->mid_joint_solver_info->current_gpose.normalize();
	}
}

template <typename Pose>
static void solve_two_bone(Pose *p_skeleton, TwoBoneIK3D::TwoBoneIK3DSetting *p_setting, const Vector3 &p_destination, const Vector3 &p_pole_destination) {
	Vector3 destination = p_destination;

	// Make vector from root to destination.
	p_setting->root_pos = p_skeleton->get_bone_global_pose(p_setting->root_bone.bone).origin; // New root position.
	Vector3 root_to_destination = destination - p_setting->root_pos;
	if (root_to_destination.is_zero_approx()) {
		return; // Abort.
	}

	double rd_len_sq = root_to_destination.length_squared();
	// Compare the distance to the target with the length of the bones.
	if (rd_len_sq >= p_setting->cached_length_sq) {
		// Result is straight.
		Vector3 rd_nrm = root_to_destination.normalized();
		p_setting->mid_pos = p_setting->root_pos + rd_nrm * p_setting->root_joint_solver_info->length;
		p_setting->end_pos = p_setting->mid_pos + rd_nrm * p_setting->mid_joint_solver_info->length;
	} else {
		// Check if the target can be reached by subtracting the lengths of the bones.
		// If not, push out target to normal of the root bone sphere.
		double sub = p_setting->root_joint_solver_info->length - p_setting->mid_joint_solver_info->length;
		if (rd_len_sq < sub * sub) {
			Vector3 push_nrm = (destination - p_setting->root_pos).normalized();
			destination = p_setting->root_pos + push_nrm * Math::abs(sub);
			root_to_destination = destination - p_setting->root_pos;
		}

		// End is snapped to the target.
		p_setting->end_pos = destination;

		// Result is bent, determine the mid position to respect the pole target.
		// Mid-position should be a point of intersection of two circles.
		double l_chain = root_to_destination.length();
		Vector3 u = root_to_destination.normalized();
		Vector3 pole_vec = SkeletonModifier3D::get_projected_normal(p_setting->root_pos, p_setting->end_pos, p_pole_destination);

		// Circle1: center is the root, radius is the length of the root bone.
		double r_root = p_setting->root_joint_solver_info->length;
		// Circle2: center is the target, radius is the length of the middle bone.
		double r_mid = p_setting->mid_joint_solver_info->length;

		double a = (l_chain * l_chain + r_root * r_root - r_mid * r_mid) / (2.0 * l_chain);
		double h2 = r_root * r_root - a * a;
		if (h2 < 0) {
			h2 = 0;
		}
		double h = Math::sqrt(h2);

		Vector3 det_plus = (p_setting->root_pos + u * a) + pole_vec * h;
		Vector3 det_minus = (p_setting->root_pos + u * a) - pole_vec * h;

		// Pick the intersection that is closest to the pole target.
		p_setting->mid_pos = p_pole_destination.distance_squared_to(det_plus) < p_pole_destination.distance_squared_to(det_minus) ? det_plus : det_minus;
	}

	// Update virtual bone rest/poses.
	cache_two_bone_vectors(p_skeleton, p_setting);
	cache_two_bone_rotations(p_skeleton, p_setting, p_pole_destination);

	// Apply the virtual bone rest/poses to the actual bones.
	p_skeleton->set_bone_pose_rotation(p_setting->root_bone.bone, p_setting->root_joint_solver_info->current_lpose);
	// Mid joint pose is relative to the root joint pose for the case root-mid or mid-end have more than 1 joints.
	p_skeleton->set_bone_pose_rotation(p_setting->middle_bone.bone, (p_skeleton->get_bone_parent(p_setting->middle_bone.bone) < 0 ? p_setting->mid_joint_solver_info->current_gpose : p_skeleton->get_bone_global_pose(p_skeleton->get_bone_parent(p_setting->middle_bone.bone)).basis.get_rotation_quaternion().inverse() * p_setting->mid_joint_solver_info->current_gpose));
}

} // namespace CZModifierSolvers
