/**************************************************************************/
/*  test_look_at_modifier_3d.cpp                                           */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_look_at_modifier_3d)

#ifndef _3D_DISABLED

#include "scene/3d/look_at_modifier_3d.h"
#include "scene/3d/skeleton_animation_pose.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

namespace TestLookAtModifier3D {

struct Rig {
	Node3D *root = nullptr;
	Skeleton3D *skeleton = nullptr;
	Node3D *target = nullptr;
	LookAtModifier3D *modifiers[3] = {};

	Rig() {
		root = memnew(Node3D);
		SceneTree::get_singleton()->get_root()->add_child(root);
		skeleton = memnew(Skeleton3D);
		root->add_child(skeleton);
		target = memnew(Node3D);
		root->add_child(target);
		skeleton->add_bone("Head");
		skeleton->add_bone("LeftEye");
		skeleton->add_bone("RightEye");
		skeleton->set_bone_parent(1, 0);
		skeleton->set_bone_parent(2, 0);
		skeleton->set_bone_rest(0, Transform3D(Basis(), Vector3(0, 2, 0)));
		skeleton->set_bone_rest(1, Transform3D(Basis(), Vector3(-0.05, 0.05, 0.1)));
		skeleton->set_bone_rest(2, Transform3D(Basis(), Vector3(0.05, 0.05, 0.1)));
		reset();
		for (int i = 0; i < 3; i++) {
			modifiers[i] = memnew(LookAtModifier3D);
			skeleton->add_child(modifiers[i]);
			modifiers[i]->set_bone(i);
			modifiers[i]->set_target_node(modifiers[i]->get_path_to(target));
		}
	}

	void reset() {
		for (int i = 0; i < 3; i++) {
			skeleton->set_bone_pose_position(i, skeleton->get_bone_rest(i).origin);
			skeleton->set_bone_pose_rotation(i, Quaternion());
		}
	}

	~Rig() {
		memdelete(root);
	}
};

TEST_CASE("[SceneTree][LookAtModifier3D] Detached head and eyes solve from current parent pose") {
	Rig rig;
	rig.target->set_position(Vector3(7, 4, 10));
	Quaternion expected[3];
	for (int i = 0; i < 3; i++) {
		rig.modifiers[i]->process_modification(0);
		expected[i] = rig.skeleton->get_bone_pose_rotation(i);
	}
	rig.reset();
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	REQUIRE(pose->capture(rig.skeleton));
	// Workers must consume the captured values, never these live scene changes.
	rig.target->set_position(Vector3(-20, -10, -15));
	rig.modifiers[0]->set_forward_axis(SkeletonModifier3D::BONE_AXIS_MINUS_Z);
	pose->evaluate();
	for (int i = 0; i < 3; i++) {
		CHECK(pose->get_bone_pose_rotation(i).is_equal_approx(expected[i]));
		Transform3D eye = pose->get_global_pose(i);
		Vector3 direction = (Vector3(7, 4, 10) - eye.origin).normalized();
		CHECK(eye.basis.get_column(2).normalized().dot(direction) > 0.99999f);
	}
	CHECK(pose->publish());
	CHECK(pose->get_fallback_reason().is_empty());
	CHECK(rig.modifiers[0]->is_target_within_limitation());
}

TEST_CASE("[SceneTree][LookAtModifier3D] Detached limits and origins match synchronous solving") {
	Rig rig;
	const Vector3 targets[] = { Vector3(8, 3, 5), Vector3(-8, 3, 5), Vector3(0, 20, 3), Vector3(0, -20, 3) };
	for (LookAtModifier3D *modifier : rig.modifiers) {
		modifier->set_relative(true);
		modifier->set_use_rest_for_limitation(true);
		modifier->set_use_angle_limitation(true);
		modifier->set_symmetry_limitation(false);
		modifier->set_primary_positive_limit_angle(Math::deg_to_rad(30.0));
		modifier->set_primary_negative_limit_angle(Math::deg_to_rad(25.0));
		modifier->set_secondary_positive_limit_angle(Math::deg_to_rad(15.0));
		modifier->set_secondary_negative_limit_angle(Math::deg_to_rad(20.0));
		modifier->set_origin_from(LookAtModifier3D::ORIGIN_FROM_SPECIFIC_BONE);
		modifier->set_origin_bone(0);
		modifier->set_origin_offset(Vector3(0, 0.03, 0));
	}
	for (const Vector3 &target : targets) {
		rig.target->set_position(target);
		rig.reset();
		Quaternion expected[3];
		for (int i = 0; i < 3; i++) {
			rig.modifiers[i]->process_modification(0);
			expected[i] = rig.skeleton->get_bone_pose_rotation(i);
		}
		rig.reset();
		Ref<SkeletonAnimationPose> pose;
		pose.instantiate();
		REQUIRE(pose->capture(rig.skeleton));
		pose->evaluate();
		for (int i = 0; i < 3; i++) {
			CHECK(pose->get_bone_pose_rotation(i).is_equal_approx(expected[i]));
		}
		CHECK(pose->publish());
		CHECK_FALSE(rig.modifiers[0]->is_target_within_limitation());
	}
}

TEST_CASE("[SceneTree][LookAtModifier3D] Time interpolation has an explicit detached fallback") {
	Rig rig;
	rig.modifiers[0]->set_duration(0.2);
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	CHECK_FALSE(pose->capture(rig.skeleton));
	CHECK(pose->get_fallback_reason().contains("duration must be zero"));
}

TEST_CASE("[SceneTree][LookAtModifier3D] Missing or coincident targets preserve finite input rotation") {
	Rig rig;
	const Quaternion input(Vector3(1, 0, 0), Math::deg_to_rad(15.0));
	rig.skeleton->set_bone_pose_rotation(0, input);
	rig.target->set_position(Vector3(0, 2, 0));
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	REQUIRE(pose->capture(rig.skeleton));
	pose->evaluate();
	CHECK(pose->get_bone_pose_rotation(0).is_equal_approx(input));
	CHECK(pose->publish());
	rig.modifiers[0]->set_target_node(NodePath("missing_target"));
	REQUIRE(pose->capture(rig.skeleton));
	pose->evaluate();
	CHECK(pose->get_bone_pose_rotation(0).is_equal_approx(input));
}

TEST_CASE("[SceneTree][LookAtModifier3D] Dependent skeleton follows final modified head") {
	Rig rig;
	rig.target->set_position(Vector3(7, 4, 10));
	Ref<SkeletonAnimationPose> source;
	source.instantiate();
	REQUIRE(source->capture(rig.skeleton));
	source->evaluate();
	Skeleton3D *hair = memnew(Skeleton3D);
	rig.root->add_child(hair);
	hair->add_bone("HeadConstraint");
	hair->set_bone_rest(0, rig.skeleton->get_bone_rest(0));
	hair->set_bone_pose_position(0, rig.skeleton->get_bone_rest(0).origin);
	Ref<SkeletonAnimationPose> dependent;
	dependent.instantiate();
	REQUIRE(dependent->capture(hair));
	dependent->configure_copy_source(source, 0, 0);
	dependent->evaluate();
	CHECK(dependent->get_global_pose(0).is_equal_approx(source->get_global_pose(0)));
	CHECK_FALSE(dependent->get_global_pose(0).is_equal_approx(source->get_base_global_pose(0)));
}

TEST_CASE("[SceneTree][LookAtModifier3D] Attached eye skeleton uses final source head frame") {
	Rig rig;
	rig.target->set_position(Vector3(7, 4, 10));
	Ref<SkeletonAnimationPose> source;
	source.instantiate();
	REQUIRE(source->capture(rig.skeleton));
	source->evaluate();
	Skeleton3D *eye_skeleton = memnew(Skeleton3D);
	rig.root->add_child(eye_skeleton);
	eye_skeleton->add_bone("Eye");
	eye_skeleton->set_bone_rest(0, Transform3D(Basis(), Vector3(0.05, 0.05, 0.1)));
	eye_skeleton->set_bone_pose_position(0, Vector3(0.05, 0.05, 0.1));
	LookAtModifier3D *eye = memnew(LookAtModifier3D);
	eye_skeleton->add_child(eye);
	eye->set_bone(0);
	eye->set_target_node(eye->get_path_to(rig.target));
	Ref<SkeletonAnimationPose> dependent;
	dependent.instantiate();
	REQUIRE(dependent->capture(eye_skeleton));
	dependent->configure_attachment(source, 0, Transform3D(), Transform3D(), false);
	dependent->evaluate();
	const Transform3D world_eye = source->get_global_pose(0) * dependent->get_global_pose(0);
	CHECK(world_eye.basis.get_column(2).normalized().dot((rig.target->get_position() - world_eye.origin).normalized()) > 0.99999f);
}

} // namespace TestLookAtModifier3D

#endif // _3D_DISABLED
