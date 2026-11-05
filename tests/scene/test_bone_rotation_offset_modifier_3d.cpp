/**************************************************************************/
/*  test_bone_rotation_offset_modifier_3d.cpp                             */
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
/* "Software"), to deal in the software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to   */
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

TEST_FORCE_LINK(test_bone_rotation_offset_modifier_3d)

#ifndef _3D_DISABLED

#include "core/object/callable_mp.h"
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/bone_rotation_offset_modifier_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/3d/skeleton_animation_pose.h"
#include "scene/3d/two_bone_ik_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#include <limits>

namespace TestBoneRotationOffsetModifier3D {

struct TestPose {
	int parents[2] = { -1, 0 };
	Transform3D local[2];
	Quaternion rotations[2];
	Vector3 scales[2] = { Vector3(1, 1, 1), Vector3(1, 1, 1) };

	int get_bone_parent(int p_bone) const {
		return parents[p_bone];
	}
	Quaternion get_bone_pose_rotation(int p_bone) const {
		return rotations[p_bone];
	}
	Transform3D get_bone_global_pose(int p_bone) const {
		const int parent = parents[p_bone];
		return parent < 0 ? local[p_bone] : get_bone_global_pose(parent) * local[p_bone];
	}
	Quaternion get_bone_global_rotation(int p_bone) const {
		const int parent = parents[p_bone];
		return parent < 0 ? rotations[p_bone] : get_bone_global_rotation(parent) * rotations[p_bone];
	}
	void set_bone_pose_rotation(int p_bone, const Quaternion &p_rotation) {
		rotations[p_bone] = p_rotation;
		local[p_bone].basis = Basis(p_rotation).scaled(scales[p_bone]);
	}
};

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Weighted local offset preserves translation and scale") {
	BoneRotationOffsetModifier3D modifier;
	CHECK_EQ(modifier.get_rotation_space(), BoneRotationOffsetModifier3D::ROTATION_SPACE_LOCAL);
	CHECK(modifier.get_rotation_offset().is_equal_approx(Quaternion()));

	TestPose pose;
	pose.local[0] = Transform3D(Basis(Quaternion(Vector3(1, 0, 0), Math::deg_to_rad(20.0))).scaled(Vector3(2, 1, 3)), Vector3(1, 2, 3));
	pose.rotations[0] = pose.local[0].basis.get_rotation_quaternion();
	pose.scales[0] = Vector3(2, 1, 3);
	const Quaternion initial = pose.get_bone_pose_rotation(0);
	const Transform3D initial_pose = pose.local[0];
	const Vector3 initial_scale = pose.scales[0];
	const Quaternion offset(Vector3(0, 1, 0), Math::deg_to_rad(90.0));
	BoneRotationOffsetModifier3D::apply(&pose, 0, offset, BoneRotationOffsetModifier3D::ROTATION_SPACE_LOCAL, 0.5f);

	CHECK(pose.get_bone_pose_rotation(0).is_equal_approx(initial * Quaternion().slerp(offset, 0.5f)));
	CHECK(pose.local[0].origin.is_equal_approx(initial_pose.origin));
	CHECK(pose.scales[0].is_equal_approx(initial_scale));
}

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Packed offsets normalize and sanitize invalid values") {
	BoneRotationOffsetModifier3D modifier;
	PackedStringArray names;
	names.push_back("finite");
	names.push_back("zero");
	names.push_back("non_finite");
	modifier.set_bone_names(names);

	const real_t nan = std::numeric_limits<real_t>::quiet_NaN();
	PackedVector4Array offsets;
	offsets.push_back(Vector4(0, 0, 2, 2));
	offsets.push_back(Vector4());
	offsets.push_back(Vector4(nan, 0, 0, 1));
	modifier.set_rotation_offsets_packed(offsets);

	const real_t component = Math::sqrt(real_t(0.5));
	CHECK(modifier.get_bone_rotation_offset(0).is_equal_approx(Quaternion(0, 0, component, component)));
	CHECK(modifier.get_bone_rotation_offset(1).is_equal_approx(Quaternion()));
	CHECK(modifier.get_bone_rotation_offset(2).is_equal_approx(Quaternion()));
}

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Packed blend endpoints normalize, interpolate, and reject mismatched sizes") {
	BoneRotationOffsetModifier3D modifier;
	PackedStringArray names;
	names.push_back("first");
	names.push_back("second");
	modifier.set_bone_names(names);

	const Quaternion from(Vector3(0, 0, 1), Math::deg_to_rad(20.0));
	const Quaternion to(Vector3(0, 0, 1), Math::deg_to_rad(100.0));
	PackedVector4Array from_samples;
	from_samples.push_back(Vector4(from.x * 2, from.y * 2, from.z * 2, from.w * 2));
	from_samples.push_back(Vector4()); // Invalid packed values sanitize to identity.
	PackedVector4Array to_samples;
	to_samples.push_back(Vector4(to.x * 3, to.y * 3, to.z * 3, to.w * 3));
	to_samples.push_back(Vector4(0, 0, 0, 2));
	CHECK(modifier.set_rotation_offset_blend_samples_packed(from_samples, to_samples));
	modifier.set_rotation_offset_blend_weight(0.25f);
	CHECK_EQ(modifier.get_rotation_offset_blend_weight(), 0.25f);
	const Quaternion expected = from.slerp(to, 0.25f).normalized();
	CHECK(modifier.get_bone_rotation_offset(0).is_equal_approx(expected));
	CHECK(modifier.get_bone_rotation_offset(1).is_equal_approx(Quaternion()));
	CHECK_EQ(modifier.get_rotation_offsets().size(), 2);
	CHECK(Quaternion(modifier.get_rotation_offsets()[0]).is_equal_approx(expected));

	PackedVector4Array mismatched_to;
	mismatched_to.push_back(Vector4(0, 0, 0, 1));
	CHECK_FALSE(modifier.set_rotation_offset_blend_samples_packed(from_samples, mismatched_to));
	CHECK(modifier.get_bone_rotation_offset(0).is_equal_approx(expected));
	modifier.set_rotation_offset_blend_weight(2.0f);
	CHECK_EQ(modifier.get_rotation_offset_blend_weight(), 1.0f);
	modifier.set_rotation_offset_blend_weight(0.25f);

	// Existing offset setters deliberately leave endpoint mode and take effect.
	PackedVector4Array direct_offsets;
	const Quaternion direct(Vector3(1, 0, 0), Math::deg_to_rad(45.0));
	direct_offsets.push_back(Vector4(direct.x, direct.y, direct.z, direct.w));
	modifier.set_rotation_offsets_packed(direct_offsets);
	CHECK(modifier.get_bone_rotation_offset(0).is_equal_approx(direct));
	CHECK(modifier.get_bone_rotation_offset(1).is_equal_approx(Quaternion()));

	CHECK(modifier.set_rotation_offset_blend_samples_packed(from_samples, to_samples));
	modifier.clear_rotation_offsets();
	CHECK(modifier.get_bone_rotation_offset(0).is_equal_approx(Quaternion()));
}

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Packed blend offsets match synchronous and detached evaluation") {
	Window *scene_root = SceneTree::get_singleton()->get_root();
	Skeleton3D *skeleton = memnew(Skeleton3D);
	scene_root->add_child(skeleton);
	skeleton->add_bone("root");
	skeleton->add_bone("child");
	skeleton->set_bone_parent(1, 0);
	skeleton->set_bone_rest(0, Transform3D());
	skeleton->set_bone_rest(1, Transform3D(Basis(), Vector3(0, 2, 0)));
	skeleton->set_bone_pose_position(1, Vector3(0, 2, 0));
	const Quaternion root_input(Vector3(1, 0, 0), Math::deg_to_rad(25.0));
	const Quaternion child_input(Vector3(0, 0, 1), Math::deg_to_rad(40.0));
	skeleton->set_bone_pose_rotation(0, root_input);
	skeleton->set_bone_pose_rotation(1, child_input);

	BoneRotationOffsetModifier3D *modifier = memnew(BoneRotationOffsetModifier3D);
	PackedStringArray names;
	names.push_back("child"); // Deliberately reverse hierarchy order.
	names.push_back("root");
	modifier->set_bone_names(names);
	modifier->set_rotation_space(BoneRotationOffsetModifier3D::ROTATION_SPACE_SKELETON);
	modifier->set_use_initial_global_pose(true);
	const Quaternion child_from(Vector3(1, 0, 0), Math::deg_to_rad(-15.0));
	const Quaternion child_to(Vector3(1, 0, 0), Math::deg_to_rad(70.0));
	const Quaternion root_from(Vector3(0, 1, 0), Math::deg_to_rad(10.0));
	const Quaternion root_to(Vector3(0, 1, 0), Math::deg_to_rad(-65.0));
	PackedVector4Array from_samples;
	from_samples.push_back(Vector4(child_from.x, child_from.y, child_from.z, child_from.w));
	from_samples.push_back(Vector4(root_from.x, root_from.y, root_from.z, root_from.w));
	PackedVector4Array to_samples;
	to_samples.push_back(Vector4(child_to.x, child_to.y, child_to.z, child_to.w));
	to_samples.push_back(Vector4(root_to.x, root_to.y, root_to.z, root_to.w));
	CHECK(modifier->set_rotation_offset_blend_samples_packed(from_samples, to_samples));
	modifier->set_rotation_offset_blend_weight(0.35f);
	skeleton->add_child(modifier);

	const Quaternion expected_child_offset = child_from.slerp(child_to, 0.35f).normalized();
	const Quaternion expected_root_offset = root_from.slerp(root_to, 0.35f).normalized();
	const Quaternion initial_root_global = skeleton->get_bone_global_pose(0).basis.get_rotation_quaternion();
	const Quaternion initial_child_global = skeleton->get_bone_global_pose(1).basis.get_rotation_quaternion();
	modifier->process_modification(0.0);
	CHECK(skeleton->get_bone_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(expected_root_offset * initial_root_global));
	CHECK(skeleton->get_bone_global_pose(1).basis.get_rotation_quaternion().is_equal_approx(expected_child_offset * initial_child_global));

	// Restore input pose. Capture resolves the blend into the detached snapshot;
	// later scalar changes cannot affect that in-flight evaluation.
	skeleton->set_bone_pose_rotation(0, root_input);
	skeleton->set_bone_pose_rotation(1, child_input);
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	CHECK(pose->capture(skeleton));
	modifier->set_rotation_offset_blend_weight(0.9f);
	Node3D *observer = memnew(Node3D);
	skeleton->add_child(observer);
	const Vector3 observed_position(3, 4, 5);
	CHECK_EQ(modifier->connect(SNAME("modification_processed"), callable_mp(observer, &Node3D::set_position).bind(observed_position)), OK);
	pose->evaluate();
	CHECK(pose->get_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(expected_root_offset * initial_root_global));
	CHECK(pose->get_global_pose(1).basis.get_rotation_quaternion().is_equal_approx(expected_child_offset * initial_child_global));
	CHECK(pose->publish());
	CHECK(observer->get_position().is_equal_approx(observed_position));
	CHECK(pose->get_fallback_reason().is_empty());

	memdelete(skeleton);
}

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Skeleton-space chain uses updated parent pose") {
	TestPose pose;
	pose.local[0] = Transform3D(Basis(Quaternion(Vector3(1, 0, 0), Math::deg_to_rad(25.0))), Vector3(0, 1, 0));
	pose.local[1] = Transform3D(Basis(Quaternion(Vector3(0, 0, 1), Math::deg_to_rad(40.0))), Vector3(0, 2, 0));
	pose.rotations[0] = pose.local[0].basis.get_rotation_quaternion();
	pose.rotations[1] = pose.local[1].basis.get_rotation_quaternion();
	const Quaternion root_before = pose.get_bone_global_rotation(0);
	const Quaternion child_before = pose.get_bone_global_rotation(1);
	const Quaternion child_local_before = pose.get_bone_pose_rotation(1);
	const Quaternion offset(Vector3(0, 1, 0), Math::deg_to_rad(70.0));
	const Quaternion root_after = offset * root_before;
	// The chain is applied in bone order. The child sees the already-rotated
	// parent global, then receives its own skeleton-space offset.
	const Quaternion child_after = offset * root_after * child_local_before;

	BoneRotationOffsetModifier3D::apply(&pose, 0, offset, BoneRotationOffsetModifier3D::ROTATION_SPACE_SKELETON, 1.0f);
	BoneRotationOffsetModifier3D::apply(&pose, 1, offset, BoneRotationOffsetModifier3D::ROTATION_SPACE_SKELETON, 1.0f);

	CHECK(pose.get_bone_global_rotation(0).is_equal_approx(root_after));
	CHECK(pose.get_bone_global_rotation(1).is_equal_approx(child_after));
	CHECK(pose.local[1].origin.is_equal_approx(Vector3(0, 2, 0)));
	CHECK(pose.scales[1].is_equal_approx(Vector3(1, 1, 1)));
}

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Per-bone offsets use one parent-ordered global snapshot") {
	Window *scene_root = SceneTree::get_singleton()->get_root();
	Skeleton3D *skeleton = memnew(Skeleton3D);
	scene_root->add_child(skeleton);
	skeleton->add_bone("root");
	skeleton->add_bone("child");
	skeleton->set_bone_parent(1, 0);
	skeleton->set_bone_rest(0, Transform3D());
	skeleton->set_bone_rest(1, Transform3D(Basis(), Vector3(0, 2, 0)));
	skeleton->set_bone_pose_position(1, Vector3(0, 2, 0));
	const Quaternion root_input(Vector3(1, 0, 0), Math::deg_to_rad(25.0));
	const Quaternion child_input(Vector3(0, 0, 1), Math::deg_to_rad(40.0));
	skeleton->set_bone_pose_rotation(0, root_input);
	skeleton->set_bone_pose_rotation(1, child_input);

	BoneRotationOffsetModifier3D *modifier = memnew(BoneRotationOffsetModifier3D);
	PackedStringArray names;
	names.push_back("child"); // Deliberately reverse hierarchy order.
	names.push_back("root");
	modifier->set_bone_names(names);
	modifier->set_rotation_space(BoneRotationOffsetModifier3D::ROTATION_SPACE_SKELETON);
	modifier->set_use_initial_global_pose(true);
	const Quaternion child_offset(Vector3(1, 1, 0).normalized(), Math::deg_to_rad(35.0));
	const Quaternion root_offset(Vector3(0, 1, 1).normalized(), Math::deg_to_rad(-55.0));
	PackedVector4Array offsets;
	offsets.push_back(Vector4(child_offset.x, child_offset.y, child_offset.z, child_offset.w));
	offsets.push_back(Vector4(root_offset.x, root_offset.y, root_offset.z, root_offset.w));
	modifier->set_rotation_offsets_packed(offsets);
	CHECK(modifier->get_bone_rotation_offset(0).is_equal_approx(child_offset));
	CHECK(modifier->get_bone_rotation_offset(1).is_equal_approx(root_offset));
	skeleton->add_child(modifier);

	const Quaternion initial_root_global = skeleton->get_bone_global_pose(0).basis.get_rotation_quaternion();
	const Quaternion initial_child_global = skeleton->get_bone_global_pose(1).basis.get_rotation_quaternion();
	modifier->process_modification(0.0);
	CHECK(skeleton->get_bone_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(root_offset * initial_root_global));
	CHECK(skeleton->get_bone_global_pose(1).basis.get_rotation_quaternion().is_equal_approx(child_offset * initial_child_global));

	// Restore the source pose and evaluate the same packed offsets on the detached path.
	skeleton->set_bone_pose_rotation(0, root_input);
	skeleton->set_bone_pose_rotation(1, child_input);
	const Vector3 initial_child_position = skeleton->get_bone_pose_position(1);
	const Vector3 initial_child_scale = skeleton->get_bone_pose_scale(1);
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	CHECK(pose->capture(skeleton));

	// Mutating the modifier after capture must not change this worker snapshot.
	const Quaternion changed_child_offset(Vector3(0, 1, 1).normalized(), Math::deg_to_rad(80.0));
	offsets.set(0, Vector4(changed_child_offset.x, changed_child_offset.y, changed_child_offset.z, changed_child_offset.w));
	modifier->set_rotation_offsets_packed(offsets);
	pose->evaluate();
	CHECK(pose->get_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(root_offset * initial_root_global));
	CHECK(pose->get_global_pose(1).basis.get_rotation_quaternion().is_equal_approx(child_offset * initial_child_global));
	CHECK(pose->get_local_pose(1).origin.is_equal_approx(initial_child_position));
	CHECK(pose->get_local_pose(1).basis.get_scale().is_equal_approx(initial_child_scale));

	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->get_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(root_offset * initial_root_global));
	CHECK(pose->get_global_pose(1).basis.get_rotation_quaternion().is_equal_approx(changed_child_offset * initial_child_global));

	memdelete(skeleton);
}

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Skeleton notifications apply partial influence once") {
	Window *scene_root = SceneTree::get_singleton()->get_root();
	for (bool initial_global : { false, true }) {
		for (real_t influence : { real_t(0), real_t(0.2), real_t(1) }) {
			Skeleton3D *skeleton = memnew(Skeleton3D);
			scene_root->add_child(skeleton);
			skeleton->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
			skeleton->add_bone("root");
			skeleton->add_bone("child");
			skeleton->set_bone_parent(1, 0);
			skeleton->set_bone_rest(0, Transform3D());
			skeleton->set_bone_rest(1, Transform3D(Basis(), Vector3(0, 2, 0)));
			skeleton->set_bone_pose_position(1, Vector3(0, 2, 0));
			const Quaternion root_input(Vector3(1, 0, 0), Math::deg_to_rad(25.0));
			const Quaternion child_input(Vector3(0, 0, 1), Math::deg_to_rad(40.0));
			skeleton->set_bone_pose_rotation(0, root_input);
			skeleton->set_bone_pose_rotation(1, child_input);

			BoneRotationOffsetModifier3D *modifier = memnew(BoneRotationOffsetModifier3D);
			PackedStringArray names;
			names.push_back("child"); // Also exercise parent ordering for the global snapshot.
			names.push_back("root");
			modifier->set_bone_names(names);
			modifier->set_rotation_space(initial_global ? BoneRotationOffsetModifier3D::ROTATION_SPACE_SKELETON : BoneRotationOffsetModifier3D::ROTATION_SPACE_LOCAL);
			modifier->set_use_initial_global_pose(initial_global);
			modifier->set_influence(influence);
			const Quaternion child_offset(Vector3(1, 1, 0).normalized(), Math::deg_to_rad(35.0));
			const Quaternion root_offset(Vector3(0, 1, 1).normalized(), Math::deg_to_rad(-55.0));
			TypedArray<Quaternion> offsets;
			offsets.push_back(child_offset);
			offsets.push_back(root_offset);
			modifier->set_rotation_offsets(offsets);
			PackedFloat32Array weights;
			weights.push_back(0.75f);
			weights.push_back(0.5f);
			modifier->set_rotation_weights(weights);
			skeleton->add_child(modifier);

			BoneAttachment3D *root_final = memnew(BoneAttachment3D);
			BoneAttachment3D *child_final = memnew(BoneAttachment3D);
			skeleton->add_child(root_final);
			skeleton->add_child(child_final);
			root_final->set_bone_idx(0);
			child_final->set_bone_idx(1);
			Ref<SkeletonAnimationPose> pose;
			pose.instantiate();
			CHECK(pose->capture(skeleton));
			pose->evaluate();

			// Exercise Skeleton3D's outer influence handling, not a direct modifier
			// call. Attachments retain the final pose after Skeleton3D restores input.
			skeleton->advance(0.0);
			skeleton->notification(Skeleton3D::NOTIFICATION_UPDATE_SKELETON);
			const Quaternion root_delta = Quaternion().slerp(root_offset, influence * 0.5f);
			const Quaternion child_delta = Quaternion().slerp(child_offset, influence * 0.75f);
			const Quaternion expected_root = initial_global ? root_delta * root_input : root_input * root_delta;
			const Quaternion expected_child = initial_global ? child_delta * root_input * child_input : expected_root * child_input * child_delta;
			CHECK(root_final->get_transform().basis.get_rotation_quaternion().is_equal_approx(expected_root));
			CHECK(child_final->get_transform().basis.get_rotation_quaternion().is_equal_approx(expected_child));
			CHECK(root_final->get_transform().is_equal_approx(pose->get_global_pose(0)));
			CHECK(child_final->get_transform().is_equal_approx(pose->get_global_pose(1)));
			CHECK(skeleton->get_bone_pose_rotation(0).is_equal_approx(root_input));
			CHECK(skeleton->get_bone_pose_rotation(1).is_equal_approx(child_input));
			memdelete(skeleton);
		}
	}
}

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Cached targets rebind on skeleton name and parent changes") {
	Window *scene_root = SceneTree::get_singleton()->get_root();
	Skeleton3D *skeleton = memnew(Skeleton3D);
	scene_root->add_child(skeleton);
	skeleton->add_bone("root");
	skeleton->add_bone("child");
	skeleton->set_bone_parent(1, 0);
	skeleton->set_bone_rest(0, Transform3D());
	skeleton->set_bone_rest(1, Transform3D(Basis(), Vector3(0, 1, 0)));
	const Quaternion root_input(Vector3(1, 0, 0), Math::deg_to_rad(20.0));
	const Quaternion child_input(Vector3(0, 0, 1), Math::deg_to_rad(-35.0));
	skeleton->set_bone_pose_rotation(0, root_input);
	skeleton->set_bone_pose_rotation(1, child_input);

	BoneRotationOffsetModifier3D *modifier = memnew(BoneRotationOffsetModifier3D);
	PackedStringArray names;
	names.push_back("root");
	names.push_back("child");
	modifier->set_bone_names(names);
	modifier->set_use_initial_global_pose(true);
	const Quaternion root_offset(Vector3(0, 1, 0), Math::deg_to_rad(50.0));
	const Quaternion child_offset(Vector3(1, 1, 0).normalized(), Math::deg_to_rad(-65.0));
	TypedArray<Quaternion> offsets;
	offsets.push_back(root_offset);
	offsets.push_back(child_offset);
	modifier->set_rotation_offsets(offsets);
	skeleton->add_child(modifier);
	modifier->process_modification(0.0);
	skeleton->set_bone_pose_rotation(0, root_input);
	skeleton->set_bone_pose_rotation(1, child_input);

	// Invert the hierarchy after the first binding. The authored target list is
	// still root then child, so the cached target order must be rebuilt child first.
	skeleton->set_bone_parent(1, -1);
	skeleton->set_bone_parent(0, 1);
	skeleton->set_bone_pose_rotation(0, root_input);
	skeleton->set_bone_pose_rotation(1, child_input);
	const Quaternion initial_root = skeleton->get_bone_global_pose(0).basis.get_rotation_quaternion();
	const Quaternion initial_child = skeleton->get_bone_global_pose(1).basis.get_rotation_quaternion();

	modifier->process_modification(0.0);
	CHECK(skeleton->get_bone_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(root_offset * initial_root));
	CHECK(skeleton->get_bone_global_pose(1).basis.get_rotation_quaternion().is_equal_approx(child_offset * initial_child));

	// The detached capture consumes the same resolved-target cache and matches
	// the synchronous path after restoring the sampled input pose.
	skeleton->set_bone_pose_rotation(0, root_input);
	skeleton->set_bone_pose_rotation(1, child_input);
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->get_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(root_offset * initial_root));
	CHECK(pose->get_global_pose(1).basis.get_rotation_quaternion().is_equal_approx(child_offset * initial_child));

	// Renaming a target must invalidate name resolution even though the index and
	// bone count are unchanged; restoring the name makes it bind again.
	skeleton->set_bone_name(1, "renamed_child");
	const Quaternion renamed_initial_root = skeleton->get_bone_global_pose(0).basis.get_rotation_quaternion();
	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->get_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(root_offset * renamed_initial_root));
	skeleton->set_bone_name(1, "child");
	const Quaternion restored_initial_root = skeleton->get_bone_global_pose(0).basis.get_rotation_quaternion();
	const Quaternion restored_initial_child = skeleton->get_bone_global_pose(1).basis.get_rotation_quaternion();
	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->get_global_pose(0).basis.get_rotation_quaternion().is_equal_approx(root_offset * restored_initial_root));
	CHECK(pose->get_global_pose(1).basis.get_rotation_quaternion().is_equal_approx(child_offset * restored_initial_child));

	memdelete(skeleton);
}

TEST_CASE("[SceneTree][BoneRotationOffsetModifier3D] Detached batch refreshes grip before TwoBone IK") {
	Window *scene_root = SceneTree::get_singleton()->get_root();
	Skeleton3D *skeleton = memnew(Skeleton3D);
	scene_root->add_child(skeleton);
	skeleton->add_bone("root");
	skeleton->add_bone("upper_arm");
	skeleton->add_bone("forearm");
	skeleton->add_bone("hand");
	skeleton->set_bone_parent(1, 0);
	skeleton->set_bone_parent(2, 1);
	skeleton->set_bone_parent(3, 2);
	skeleton->set_bone_rest(0, Transform3D());
	skeleton->set_bone_rest(1, Transform3D(Basis(), Vector3(0, 1, 0)));
	skeleton->set_bone_rest(2, Transform3D(Basis(), Vector3(1, 0, 0)));
	skeleton->set_bone_rest(3, Transform3D(Basis(), Vector3(1, 0, 0)));
	// SkeletonAnimationPose captures current pose translations; rests only define
	// the solver's segment lengths. Keep the animated chain aligned with those rests.
	skeleton->set_bone_pose_position(0, Vector3());
	skeleton->set_bone_pose_position(1, Vector3(0, 1, 0));
	skeleton->set_bone_pose_position(2, Vector3(1, 0, 0));
	skeleton->set_bone_pose_position(3, Vector3(1, 0, 0));

	Node3D *grip_target = memnew(Node3D);
	grip_target->set_name("GripTarget");
	skeleton->add_child(grip_target);
	Node3D *pole = memnew(Node3D);
	pole->set_name("Pole");
	pole->set_position(Vector3(1, 1, 1));
	skeleton->add_child(pole);

	BoneRotationOffsetModifier3D *rotation = memnew(BoneRotationOffsetModifier3D);
	PackedStringArray names;
	names.push_back("upper_arm");
	rotation->set_bone_names(names);
	rotation->set_rotation_offset(Quaternion(Vector3(0, 0, 1), Math::deg_to_rad(90.0)));
	skeleton->add_child(rotation);

	TwoBoneIK3D *ik = memnew(TwoBoneIK3D);
	ik->set_setting_count(1);
	ik->set_root_bone(0, 1);
	ik->set_middle_bone(0, 2);
	ik->set_end_bone(0, 3);
	ik->set_target_node(0, NodePath("../GripTarget"));
	ik->set_pole_node(0, NodePath("../Pole"));
	skeleton->add_child(ik);
	Dictionary aim_input;
	aim_input["hand_bone"] = 3;
	aim_input["hand_to_grip"] = Transform3D(Basis(), Vector3(-0.25, 0, 0));
	aim_input["grip_target_id"] = uint64_t(grip_target->get_instance_id());
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	pose->configure_aim(0, aim_input);
	CHECK(pose->capture(skeleton));
	pose->evaluate();

	const Vector3 expected_grip = Vector3(0, 2.75, 0);
	CHECK(pose->publish());
	CHECK_EQ(grip_target->get_position(), expected_grip);
	CHECK(pose->get_global_pose(3).origin.is_equal_approx(expected_grip));

	memdelete(skeleton);
}

TEST_CASE("[SceneTree][SkeletonAnimationPose] Grip target crossing preserves a nearly extended arm bend side") {
	Window *scene_root = SceneTree::get_singleton()->get_root();
	const Vector3 source_elbow(0.99, -0.01, 0);
	const Vector3 source_hand(1.98, 0, 0);
	const Vector3 weapon_hand(0, 0, 1);
	for (int source_hand_index : { -1, 2, 3 }) {
		for (bool use_aim_grip : { false, true }) {
			for (real_t target_y : { real_t(-0.2), real_t(0.2) }) {
				INFO("aim grip: ", use_aim_grip, ", target y: ", target_y, ", source hand: ", source_hand_index);
				Skeleton3D *skeleton = memnew(Skeleton3D);
				scene_root->add_child(skeleton);
				for (const char *name : { "upper_arm", "forearm", "left_hand", "weapon_hand" }) {
					skeleton->add_bone(name);
				}
				skeleton->set_bone_parent(1, 0);
				skeleton->set_bone_parent(2, 1);
				const Vector3 positions[] = { Vector3(), source_elbow, source_hand - source_elbow, weapon_hand };
				for (int bone = 0; bone < 4; bone++) {
					skeleton->set_bone_rest(bone, Transform3D(Basis(), positions[bone]));
					skeleton->set_bone_pose_position(bone, positions[bone]);
				}
				Node3D *grip_target = memnew(Node3D);
				grip_target->set_name("GripTarget");
				skeleton->add_child(grip_target);
				Node3D *pole = memnew(Node3D);
				pole->set_name("ElbowPole");
				skeleton->add_child(pole);
				TwoBoneIK3D *ik = memnew(TwoBoneIK3D);
				ik->set_setting_count(1);
				ik->set_root_bone(0, 0);
				ik->set_middle_bone(0, 1);
				ik->set_end_bone(0, 2);
				ik->set_target_node(0, NodePath("../GripTarget"));
				ik->set_pole_node(0, NodePath("../ElbowPole"));
				ik->set_pole_direction(0, SkeletonModifier3D::SECONDARY_DIRECTION_NONE);
				skeleton->add_child(ik);
				const Vector3 target(1.8, target_y, 0);
				const Vector3 explicit_pole(0.9, -2, 0);
				const bool use_explicit_pole = source_hand_index == 3;
				Dictionary input;
				if (source_hand_index >= 0) {
					input["left_hand_bone"] = 2;
				}
				input["elbow_pole_id"] = uint64_t(pole->get_instance_id());
				Ref<SkeletonAnimationPose> pose;
				pose.instantiate();
				if (use_explicit_pole) {
					input["hand_bone"] = 3;
					input["hand_to_elbow_pole"] = explicit_pole - weapon_hand;
					pose->configure_aim(0, input);
				}
				if (use_aim_grip) {
					input["hand_bone"] = 3;
					input["left_arm_bone"] = 0;
					input["left_elbow_bone"] = 1;
					input["hand_to_grip"] = Transform3D(Basis(), target - weapon_hand);
					input["grip_target_id"] = uint64_t(grip_target->get_instance_id());
					pose->configure_aim(0, input);
				} else {
					for (const char *key : { "hips_bone", "left_foot_bone", "right_foot_bone", "left_toes_bone", "right_toes_bone", "left_upper_leg_bone", "left_lower_leg_bone", "right_upper_leg_bone", "right_lower_leg_bone" }) {
						input[key] = 0;
					}
					input["left_upper_arm_bone"] = 0;
					input["left_lower_arm_bone"] = 1;
					input["left_arm_rest_pole"] = Vector3(0, -1, 0);
					input["has_hand_target"] = true;
					input["left_hand_weight"] = 1.0;
					input["hand_target_world"] = Transform3D(Basis(), target);
					input["hand_target_id"] = uint64_t(grip_target->get_instance_id());
					pose->configure_ik(input);
				}
				CHECK(pose->capture(skeleton));
				pose->evaluate();
				CHECK(pose->publish());
				const Vector3 target_axis = target.normalized();
				// Both destinations keep the elbow on the authored side of the arm,
				// even when projecting its old position onto the new axis would flip it.
				Vector3 expected_side(target_axis.y, -target_axis.x, 0);
				// The source hand is optional. Existing callers retain their previous
				// projected-elbow behavior until they supply the animation hand bone.
				if (source_hand_index < 0 && target_y < 0) {
					expected_side = -expected_side;
				}
				CHECK(pole->get_position().dot(expected_side) > 0.5);
				if (use_explicit_pole) {
					CHECK(pole->get_position().distance_to(explicit_pole) < 0.0001);
				}
				CHECK(pose->get_global_pose(1).origin.dot(expected_side) > 0.1);
				CHECK(pose->get_global_pose(2).origin.distance_to(target) < 0.0001);
				CHECK(pose->get_fallback_reason().is_empty());
				memdelete(skeleton);
			}
		}
	}
}

TEST_CASE("[SceneTree][SkeletonAnimationPose] Ground-motion IK preserves toe swing on raw contact plane") {
	Window *scene_root = SceneTree::get_singleton()->get_root();
	Skeleton3D *skeleton = memnew(Skeleton3D);
	scene_root->add_child(skeleton);
	const char *names[] = { "hips", "left_upper_leg", "left_lower_leg", "left_foot", "left_toes", "right_upper_leg", "right_lower_leg", "right_foot", "right_toes" };
	for (const char *name : names) {
		skeleton->add_bone(name);
	}
	skeleton->set_bone_parent(1, 0);
	skeleton->set_bone_parent(2, 1);
	skeleton->set_bone_parent(3, 2);
	skeleton->set_bone_parent(4, 3);
	skeleton->set_bone_parent(5, 0);
	skeleton->set_bone_parent(6, 5);
	skeleton->set_bone_parent(7, 6);
	skeleton->set_bone_parent(8, 7);
	const Vector3 hips_position(0, 1, 0);
	const Vector3 upper_position(0, -0.3, 0);
	const Vector3 lower_position(0, -0.3, 0);
	const Vector3 foot_position(0, -0.3, 0);
	const Vector3 toe_rest_position(0.2, 0, 0.3);
	skeleton->set_bone_rest(0, Transform3D(Basis(), hips_position));
	skeleton->set_bone_rest(1, Transform3D(Basis(), upper_position));
	skeleton->set_bone_rest(2, Transform3D(Basis(), lower_position));
	skeleton->set_bone_rest(3, Transform3D(Basis(), foot_position));
	skeleton->set_bone_rest(4, Transform3D(Basis(), toe_rest_position));
	skeleton->set_bone_rest(5, Transform3D(Basis(), upper_position));
	skeleton->set_bone_rest(6, Transform3D(Basis(), lower_position));
	skeleton->set_bone_rest(7, Transform3D(Basis(), foot_position));
	skeleton->set_bone_rest(8, Transform3D(Basis(), toe_rest_position));
	for (int bone = 0; bone < skeleton->get_bone_count(); bone++) {
		skeleton->set_bone_pose_position(bone, skeleton->get_bone_rest(bone).origin);
	}
	const real_t source_swing = 0.1;
	skeleton->set_bone_pose_position(4, toe_rest_position + Vector3(0, source_swing, 0));

	Node3D *pelvis_target = memnew(Node3D);
	Node3D *left_target = memnew(Node3D);
	Node3D *left_pole = memnew(Node3D);
	skeleton->add_child(pelvis_target);
	skeleton->add_child(left_target);
	skeleton->add_child(left_pole);

	Dictionary input;
	input["hips_bone"] = 0;
	input["left_upper_arm_bone"] = 1;
	input["left_lower_arm_bone"] = 2;
	input["left_foot_bone"] = 3;
	input["right_foot_bone"] = 7;
	input["left_toes_bone"] = 4;
	input["right_toes_bone"] = 8;
	input["left_upper_leg_bone"] = 1;
	input["left_lower_leg_bone"] = 2;
	input["right_upper_leg_bone"] = 5;
	input["right_lower_leg_bone"] = 6;
	input["left_leg_length"] = 1.0;
	input["right_leg_length"] = 1.0;
	input["maximum_ground_slope_degrees"] = 45.0;
	input["sole_offset_cm"] = 0.02;
	input["foot_source_stance_calibration_cm"] = 0.03;
	input["ground_motion_contact"] = true;
	input["left_foot_weight"] = 1.0;
	input["right_foot_weight"] = 0.0;
	input["pelvis_target_id"] = uint64_t(pelvis_target->get_instance_id());
	input["left_foot_target_id"] = uint64_t(left_target->get_instance_id());
	input["left_knee_pole_id"] = uint64_t(left_pole->get_instance_id());
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	pose->configure_ik(input);
	const real_t raw_angle = Math::deg_to_rad(30.0);
	const Vector3 raw_normal(Math::sin(raw_angle), Math::cos(raw_angle), 0);
	pose->set_ik_ground_motion_input(true, Vector3(0, 0, 0), raw_normal, Vector3(0, 1, 0), false, Vector3(), Vector3(), Vector3(0, 1, 0));
	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->publish());

	const Transform3D foot_target_world = left_target->get_global_transform();
	const Vector3 target_toe_world = foot_target_world.xform(toe_rest_position + Vector3(0, source_swing, 0));
	const real_t raw_plane_height = -raw_normal.x * target_toe_world.x / raw_normal.y;
	const real_t expected_toe_height = raw_plane_height + 0.02 + source_swing - 0.03;
	CHECK(Math::is_equal_approx(target_toe_world.y, expected_toe_height));
	CHECK(foot_target_world.basis.is_equal_approx(Basis())); // Smoothed normal, not the raw slope, orients the foot.
	CHECK(pose->get_fallback_reason().is_empty());
	const real_t negative_source_swing = -0.1;
	skeleton->set_bone_pose_position(4, toe_rest_position + Vector3(0, negative_source_swing, 0));
	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->publish());
	const Transform3D negative_swing_target = left_target->get_global_transform();
	const Vector3 negative_swing_toe_world = negative_swing_target.xform(toe_rest_position + Vector3(0, negative_source_swing, 0));
	const real_t negative_swing_plane_height = -raw_normal.x * negative_swing_toe_world.x / raw_normal.y;
	const real_t expected_negative_swing_height = negative_swing_plane_height + 0.02 + MAX(real_t(0), negative_source_swing - 0.03);
	CHECK(Math::is_equal_approx(negative_swing_toe_world.y, expected_negative_swing_height));
	CHECK(negative_swing_toe_world.y >= negative_swing_plane_height + 0.02 - real_t(0.00001));
	const real_t boundary_angle = Math::deg_to_rad(45.0);
	const Vector3 boundary_normal(Math::sin(boundary_angle), Math::cos(boundary_angle), 0);
	pose->set_ik_ground_motion_input(true, Vector3(), boundary_normal, Vector3(0, 1, 0), false, Vector3(), Vector3(), Vector3(0, 1, 0));
	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->publish());
	const Transform3D boundary_target = left_target->get_global_transform();
	CHECK(!boundary_target.is_equal_approx(negative_swing_target)); // Exactly the configured slope limit remains usable.
	const real_t over_limit_angle = Math::deg_to_rad(46.0);
	pose->set_ik_ground_motion_input(true, Vector3(), Vector3(Math::sin(over_limit_angle), Math::cos(over_limit_angle), 0), Vector3(0, 1, 0), false, Vector3(), Vector3(), Vector3(0, 1, 0));
	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->publish());
	CHECK(left_target->get_global_transform().is_equal_approx(boundary_target)); // Slopes above the limit are rejected.
	pose->set_ik_ground_motion_input(true, Vector3(), Vector3(0.8660254, 0.5, 0), Vector3(0, 1, 0), false, Vector3(), Vector3(), Vector3(0, 1, 0));
	CHECK(pose->capture(skeleton));
	pose->evaluate();
	CHECK(pose->publish());
	CHECK(left_target->get_global_transform().is_equal_approx(boundary_target)); // Raw plane slope controls validity.

	memdelete(skeleton);
}

} // namespace TestBoneRotationOffsetModifier3D

#endif // _3D_DISABLED
