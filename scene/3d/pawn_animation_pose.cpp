/**************************************************************************/
/*  pawn_animation_pose.cpp                                                */
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

#include "pawn_animation_pose.h"

#include "core/os/thread.h"
#include "scene/3d/skeleton_animation_pose.h"

void PawnAnimationPose::configure_aim(uint64_t p_modifier_id, const Dictionary &p_input) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	aim.modifier_id = ObjectID(p_modifier_id);
	aim.aim = p_input.get("aim_bone", -1);
	aim.hand = p_input.get("hand_bone", -1);
	aim.hips = p_input.get("hips_bone", -1);
	aim.chest = p_input.get("chest_bone", -1);
	aim.upper_chest = p_input.get("upper_chest_bone", -1);
	aim.left_leg = p_input.get("left_leg_bone", -1);
	aim.right_leg = p_input.get("right_leg_bone", -1);
	aim.left_arm = p_input.get("left_arm_bone", -1);
	aim.left_elbow = p_input.get("left_elbow_bone", -1);
	aim.hip_weight = p_input.get("hip_yaw_weight", 0.0);
	aim.hand_to_muzzle = p_input.get("hand_to_muzzle", Transform3D());
	aim.has_grip = p_input.has("hand_to_grip");
	aim.hand_to_grip = p_input.get("hand_to_grip", Transform3D());
	aim.target = p_input.get("target_world", Vector3());
	aim.up = p_input.get("up_world", Vector3(0, 1, 0));
	aim.grip_target_id = ObjectID(uint64_t(p_input.get("grip_target_id", uint64_t(0))));
	aim.elbow_pole_id = ObjectID(uint64_t(p_input.get("elbow_pole_id", uint64_t(0))));
}

void PawnAnimationPose::rotate_global(int p_bone, const Quaternion &p_rotation) {
	Transform3D bone_pose = pose.get_global_pose(p_bone);
	bone_pose.basis = Basis(p_rotation) * bone_pose.basis;
	pose.set_bone_global_pose(p_bone, bone_pose);
}

void PawnAnimationPose::solve_aim() {
	aim_evaluated = true;
	Vector3 target = pose.world.affine_inverse().xform(aim.target);
	Vector3 up = pose.world.basis.inverse().xform(aim.up).normalized();
	Transform3D left_leg = pose.get_global_pose(aim.left_leg);
	Transform3D right_leg = pose.get_global_pose(aim.right_leg);
	for (int iteration = 0; iteration < 4; iteration++) {
		Transform3D muzzle = pose.get_global_pose(aim.hand) * aim.hand_to_muzzle;
		Vector3 desired = target - muzzle.origin;
		if (desired.length_squared() < 0.000001f) {
			break;
		}
		Vector3 barrel = muzzle.basis.get_column(2).normalized();
		desired.normalize();
		real_t yaw_angle = Math::atan2(up.dot(barrel.cross(desired)), barrel.dot(desired) - barrel.dot(up) * desired.dot(up));
		Quaternion yaw(up, yaw_angle);
		Vector3 pitch_axis = desired.cross(up).normalized();
		Quaternion pitch(pitch_axis, yaw.xform(barrel).signed_angle_to(desired, pitch_axis));
		pitch = (pitch * yaw).normalized() * yaw.inverse();
		real_t weight = (1 - aim.hip_weight) / 3;
		rotate_global(aim.hips, Quaternion(up, yaw_angle * aim.hip_weight));
		rotate_global(aim.aim, Quaternion(up, yaw_angle * weight));
		rotate_global(aim.chest, Quaternion(up, yaw_angle * weight));
		rotate_global(aim.upper_chest, Quaternion(up, yaw_angle * weight));
		rotate_global(aim.aim, pitch);
	}
	pose.set_bone_global_pose(aim.left_leg, left_leg);
	pose.set_bone_global_pose(aim.right_leg, right_leg);
	Transform3D hand = pose.get_global_pose(aim.hand);
	aim.muzzle = pose.world * hand * aim.hand_to_muzzle;
	aim.error_degrees = Math::rad_to_deg(aim.muzzle.basis.get_column(2).normalized().angle_to((aim.target - aim.muzzle.origin).normalized()));
	if (!aim.has_grip) {
		return;
	}
	aim.grip = hand * aim.hand_to_grip;
	bool has_pole = aim.left_arm >= 0 && aim.left_elbow >= 0;
	if (has_pole) {
		aim.elbow_pole = calculate_pole(pose.get_global_pose(aim.left_arm).origin,
				pose.get_global_pose(aim.left_elbow).origin, aim.grip.origin, Vector3(0, 0, 1));
	}
	// Later modifiers consume the freshly corrected grip, matching PawnAimModifier.UpdateGripTarget.
	for (auto &in : pose.two_bone) {
		if (in.target_id == aim.grip_target_id) {
			in.target = aim.grip.origin;
		}
		if (has_pole && in.pole_id == aim.elbow_pole_id) {
			in.pole = aim.elbow_pole;
		}
	}
	for (auto &in : pose.copies) {
		if (in.target_id == aim.grip_target_id) {
			in.target = aim.grip;
		}
	}
}

void PawnAnimationPose::generate_grip_target() {
	if (!aim.has_grip || aim.hand < 0 || aim.hand >= int(pose.bones.size())) { return; }
	const Transform3D grip = pose.get_global_pose(aim.hand) * aim.hand_to_grip;
	pose.set_target(aim.grip_target_id, grip);
	if (aim.left_arm >= 0 && aim.left_elbow >= 0) {
		const Vector3 pole = calculate_pole(pose.get_global_pose(aim.left_arm).origin, pose.get_global_pose(aim.left_elbow).origin, grip.origin, Vector3(0, 0, 1));
		pose.set_target(aim.elbow_pole_id, Transform3D(Basis(), pole), true);
	}
}

void PawnAnimationPose::set_aim_input(real_t p_hip_weight, const Transform3D &p_muzzle, bool p_has_grip, const Transform3D &p_grip, const Vector3 &p_target, const Vector3 &p_up) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	aim.hip_weight = p_hip_weight;
	aim.hand_to_muzzle = p_muzzle;
	aim.has_grip = p_has_grip;
	aim.hand_to_grip = p_grip;
	aim.target = p_target;
	aim.up = p_up;
}

void PawnAnimationPose::set_ik_hand_input(bool p_has_target, const Transform3D &p_target, const Vector3 &p_weights) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ik.has_hand_target = p_has_target;
	ik.hand_target_world = p_target;
	ik.hand_weight = p_weights.x;
	ik.left_weight = p_weights.y;
	ik.right_weight = p_weights.z;
}

void PawnAnimationPose::set_ik_ground_input(bool p_left_hit, const Vector3 &p_left_position, const Vector3 &p_left_normal, bool p_right_hit, const Vector3 &p_right_position, const Vector3 &p_right_normal) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ik.left_ground = { p_left_hit, p_left_position, p_left_normal };
	ik.right_ground = { p_right_hit, p_right_position, p_right_normal };
}

void PawnAnimationPose::configure_ik(const Dictionary &p_input) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ik.configured = !p_input.is_empty();
	if (!ik.configured) { return; }
	pose.generated_targets.reserve(7);
	ik.has_hand_target = p_input.get("has_hand_target", false);
	ik.hand_target_world = p_input.get("hand_target_world", Transform3D());
	ik.hand_weight = p_input.get("left_hand_weight", 0.0);
	ik.left_weight = p_input.get("left_foot_weight", 0.0);
	ik.right_weight = p_input.get("right_foot_weight", 0.0);
	ik.hips = p_input.get("hips_bone", -1);
	ik.left_upper_arm = p_input.get("left_upper_arm_bone", -1);
	ik.left_lower_arm = p_input.get("left_lower_arm_bone", -1);
	ik.left_foot = p_input.get("left_foot_bone", -1);
	ik.right_foot = p_input.get("right_foot_bone", -1);
	ik.left_toes = p_input.get("left_toes_bone", -1);
	ik.right_toes = p_input.get("right_toes_bone", -1);
	ik.left_upper_leg = p_input.get("left_upper_leg_bone", -1);
	ik.left_lower_leg = p_input.get("left_lower_leg_bone", -1);
	ik.right_upper_leg = p_input.get("right_upper_leg_bone", -1);
	ik.right_lower_leg = p_input.get("right_lower_leg_bone", -1);
	ik.arm_rest_pole = p_input.get("left_arm_rest_pole", Vector3());
	ik.left_rest_pole = p_input.get("left_leg_rest_pole", Vector3());
	ik.right_rest_pole = p_input.get("right_leg_rest_pole", Vector3());
	ik.left_length = p_input.get("left_leg_length", 0.0);
	ik.right_length = p_input.get("right_leg_length", 0.0);
	ik.reach_epsilon = p_input.get("leg_reach_epsilon", 0.0);
	ik.maximum_slope = p_input.get("maximum_ground_slope_degrees", 0.0);
	ik.sole_offset = p_input.get("sole_offset_cm", 0.0);
	ik.pelvis_drop = p_input.get("maximum_pelvis_drop_cm", 0.0);
	ik.pelvis_raise = p_input.get("maximum_pelvis_raise_cm", 0.0);
	ik.left_ground.hit = p_input.get("left_ground_has_hit", false);
	ik.right_ground.hit = p_input.get("right_ground_has_hit", false);
	ik.left_ground.position = p_input.get("left_ground_position", Vector3());
	ik.left_ground.normal = p_input.get("left_ground_normal", Vector3());
	ik.right_ground.position = p_input.get("right_ground_position", Vector3());
	ik.right_ground.normal = p_input.get("right_ground_normal", Vector3());
	ik.hand_target = ObjectID(uint64_t(p_input.get("hand_target_id", uint64_t(0))));
	ik.elbow_pole = ObjectID(uint64_t(p_input.get("elbow_pole_id", uint64_t(0))));
	ik.pelvis_target = ObjectID(uint64_t(p_input.get("pelvis_target_id", uint64_t(0))));
	ik.left_target = ObjectID(uint64_t(p_input.get("left_foot_target_id", uint64_t(0))));
	ik.left_pole = ObjectID(uint64_t(p_input.get("left_knee_pole_id", uint64_t(0))));
	ik.right_target = ObjectID(uint64_t(p_input.get("right_foot_target_id", uint64_t(0))));
	ik.right_pole = ObjectID(uint64_t(p_input.get("right_knee_pole_id", uint64_t(0))));
}

Vector3 PawnAnimationPose::calculate_pole(const Vector3 &p_root, const Vector3 &p_middle, const Vector3 &p_target, const Vector3 &p_rest) {
	Vector3 axis = p_target - p_root;
	if (axis.length_squared() < 0.000001f) {
		return p_middle + p_rest;
	}
	Vector3 projected = p_root + axis * ((p_middle - p_root).dot(axis) / axis.length_squared());
	Vector3 bend = p_middle - projected;
	if (bend.length_squared() < 0.000001f) { bend = p_rest; }
	real_t distance = MAX((p_middle - p_root).length() + (p_target - p_middle).length(), real_t(0.1));
	return p_middle + bend.normalized() * distance;
}

bool PawnAnimationPose::create_foot_target(const Transform3D &p_pose, const GroundInput &p_ground, int p_toes, Transform3D &r_target) {
	if (!p_ground.hit || p_ground.normal.length_squared() < 0.000001f) { return false; }
	Vector3 normal = p_ground.normal.normalized();
	real_t slope = Math::rad_to_deg(Math::acos(CLAMP(normal.dot(Vector3(0, 1, 0)), real_t(-1), real_t(1))));
	if (slope > ik.maximum_slope) { return false; }
	Transform3D foot_world = pose.world * p_pose;
	Quaternion alignment(Vector3(0, 1, 0), normal);
	foot_world.basis = (Basis(alignment) * foot_world.basis.orthonormalized()).orthonormalized();
	real_t ground_height = p_ground.position.y - (normal.x * (foot_world.origin.x - p_ground.position.x) + normal.z * (foot_world.origin.z - p_ground.position.z)) / normal.y;
	real_t reference_height = (pose.world * pose.bones[p_toes].global_rest).origin.y;
	foot_world.origin += Vector3(0, 1, 0) * (ground_height - reference_height + ik.sole_offset);
	r_target = pose.world.affine_inverse() * foot_world;
	return true;
}

void PawnAnimationPose::generate_ik_targets() {
	if (!ik.configured) { return; }
	constexpr real_t blend_epsilon = 0.0001f;
	if (ik.hand_weight > blend_epsilon && ik.has_hand_target) {
		Transform3D target = pose.world.affine_inverse() * ik.hand_target_world;
		pose.set_target(ik.hand_target, target);
		Vector3 pole = calculate_pole(pose.get_global_pose(ik.left_upper_arm).origin, pose.get_global_pose(ik.left_lower_arm).origin, target.origin, ik.arm_rest_pole);
		pose.set_target(ik.elbow_pole, Transform3D(Basis(), pole), true);
	}
	if (MAX(ik.left_weight, ik.right_weight) <= blend_epsilon) { return; }
	Transform3D left_pose = pose.get_global_pose(ik.left_foot);
	Transform3D right_pose = pose.get_global_pose(ik.right_foot);
	Transform3D left, right;
	bool has_left = ik.left_weight > blend_epsilon && create_foot_target(left_pose, ik.left_ground, ik.left_toes, left);
	bool has_right = ik.right_weight > blend_epsilon && create_foot_target(right_pose, ik.right_ground, ik.right_toes, right);
	real_t left_offset = has_left ? (pose.world.xform(left.origin) - pose.world.xform(left_pose.origin)).y : 0;
	real_t right_offset = has_right ? (pose.world.xform(right.origin) - pose.world.xform(right_pose.origin)).y : 0;
	real_t pelvis_offset = has_left && has_right ? MIN(left_offset, right_offset) : has_left ? left_offset : right_offset;
	pelvis_offset = CLAMP(pelvis_offset, -ik.pelvis_drop, ik.pelvis_raise);
	Transform3D hips = pose.get_global_pose(ik.hips);
	Vector3 hips_origin = hips.origin;
	hips.origin = pose.world.affine_inverse().xform(pose.world.xform(hips.origin) + Vector3(0, 1, 0) * pelvis_offset);
	pose.set_target(ik.pelvis_target, hips);
	Vector3 translation = hips.origin - hips_origin;
	auto clamp_reach = [](const Vector3 &root, const Vector3 &target, real_t length, real_t epsilon) {
		Vector3 offset = target - root;
		real_t reach = MAX(length - epsilon, real_t(0));
		return offset.length_squared() <= reach * reach || offset.length_squared() < 0.000001f ? target : root + offset.normalized() * reach;
	};
	if (has_left) {
		Vector3 root = pose.get_global_pose(ik.left_upper_leg).origin + translation;
		Vector3 middle = pose.get_global_pose(ik.left_lower_leg).origin + translation;
		left.origin = clamp_reach(root, left.origin, ik.left_length, ik.reach_epsilon);
		pose.set_target(ik.left_target, left);
		pose.set_target(ik.left_pole, Transform3D(Basis(), calculate_pole(root, middle, left_pose.origin + translation, ik.left_rest_pole)), true);
	}
	if (has_right) {
		Vector3 root = pose.get_global_pose(ik.right_upper_leg).origin + translation;
		Vector3 middle = pose.get_global_pose(ik.right_lower_leg).origin + translation;
		right.origin = clamp_reach(root, right.origin, ik.right_length, ik.reach_epsilon);
		pose.set_target(ik.right_target, right);
		pose.set_target(ik.right_pole, Transform3D(Basis(), calculate_pole(root, middle, right_pose.origin + translation, ik.right_rest_pole)), true);
	}
}

bool PawnAnimationPose::validate_ik_bones(int p_count) const {
	if (!ik.configured) { return true; }
	for (int bone : { ik.hips, ik.left_upper_arm, ik.left_lower_arm, ik.left_foot, ik.right_foot, ik.left_toes, ik.right_toes, ik.left_upper_leg, ik.left_lower_leg, ik.right_upper_leg, ik.right_lower_leg }) {
		if (bone < 0 || bone >= p_count) { return false; }
	}
	return true;
}

bool PawnAnimationPose::validate_aim_bones(int p_count) const {
	for (int bone : { aim.aim, aim.hand, aim.hips, aim.chest, aim.upper_chest, aim.left_leg, aim.right_leg }) {
		if (bone < 0 || bone >= p_count) { return false; }
	}
	return true;
}

void PawnAnimationPose::generate_targets() {
	generate_ik_targets();
	generate_grip_target();
}

uint32_t PawnAnimationPose::publish_targets() {
	uint32_t writes = 0;
	if (aim_evaluated && aim.has_grip) {
		if (auto *target = Object::cast_to<Node3D>(ObjectDB::get_instance(aim.grip_target_id))) {
			writes++;
			target->set_transform(aim.grip);
		}
		if (aim.left_arm >= 0 && aim.left_elbow >= 0) {
			if (auto *pole = Object::cast_to<Node3D>(ObjectDB::get_instance(aim.elbow_pole_id))) {
				writes++;
				pole->set_position(aim.elbow_pole);
			}
		}
	}
	return writes;
}
