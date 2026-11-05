/**************************************************************************/
/*  pawn_animation_pose.h                                                */
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

#include "core/math/transform_3d.h"
#include "core/object/object_id.h"
#include "core/variant/dictionary.h"

class SkeletonAnimationPose;

// ColonyZero's humanoid aim and hand/foot target rules. The owning pose captures
// inputs on main, evaluates on one worker, and publishes targets on main.
// Keep these rules separate from generic skeleton storage and native IK solvers.
class PawnAnimationPose {
	SkeletonAnimationPose &pose;
	struct AimInput {
		ObjectID modifier_id, grip_target_id, elbow_pole_id;
		int aim = -1, hand = -1, hips = -1, chest = -1, upper_chest = -1;
		int left_leg = -1, right_leg = -1, left_arm = -1, left_elbow = -1;
		real_t hip_weight = 0;
		Transform3D hand_to_muzzle, hand_to_grip, muzzle, grip;
		Vector3 target, up = Vector3(0, 1, 0), elbow_pole;
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
	bool aim_evaluated = false;
	void rotate_global(int p_bone, const Quaternion &p_rotation);
	void generate_grip_target();
	void generate_ik_targets();
	bool create_foot_target(const Transform3D &p_pose, const GroundInput &p_ground, int p_toes, Transform3D &r_target);
	static Vector3 calculate_pole(const Vector3 &p_root, const Vector3 &p_middle, const Vector3 &p_target, const Vector3 &p_rest);

public:
	explicit PawnAnimationPose(SkeletonAnimationPose &p_pose) : pose(p_pose) {}
	bool is_aim_modifier(ObjectID p_id) const { return p_id == aim.modifier_id; }
	bool has_aim_bones() const { return aim.aim >= 0 && aim.hand >= 0; }
	bool validate_aim_bones(int p_count) const;
	bool validate_ik_bones(int p_count) const;
	void reset_frame() { aim_evaluated = false; }
	void generate_targets();
	void solve_aim();
	uint32_t publish_targets();
	Transform3D get_aim_muzzle() const { return aim.muzzle; }
	real_t get_aim_error_degrees() const { return aim.error_degrees; }
	void configure_aim(uint64_t p_modifier_id, const Dictionary &p_input);
	void configure_ik(const Dictionary &p_input);
	void set_aim_input(real_t p_hip_weight, const Transform3D &p_muzzle, bool p_has_grip, const Transform3D &p_grip, const Vector3 &p_target, const Vector3 &p_up);
	void set_ik_hand_input(bool p_has_target, const Transform3D &p_target, const Vector3 &p_weights);
	void set_ik_ground_input(bool p_left_hit, const Vector3 &p_left_position, const Vector3 &p_left_normal, bool p_right_hit, const Vector3 &p_right_position, const Vector3 &p_right_normal);
};
