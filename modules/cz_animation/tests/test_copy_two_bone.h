/**************************************************************************/
/*  test_copy_two_bone.h                                                  */
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

#include "core/object/callable_mp.h"
#include "core/os/thread.h"
#include "scene/3d/copy_transform_modifier_3d.h"
#include "scene/3d/two_bone_ik_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "tests/test_macros.h"

#include "modules/cz_animation/skeleton_animation_pose.h"

namespace TestCZModifierSolvers {

class FinalPoseRecorder : public RefCounted {
	GDCLASS(FinalPoseRecorder, RefCounted);

protected:
	static void _bind_methods() {}

public:
	Skeleton3D *skeleton = nullptr;
	LocalVector<Transform3D> local;
	LocalVector<Transform3D> global;
	int notifications = 0;

	void record() {
		notifications++;
		local.resize(skeleton->get_bone_count());
		global.resize(skeleton->get_bone_count());
		for (int bone = 0; bone < skeleton->get_bone_count(); bone++) {
			local[bone] = skeleton->get_bone_pose(bone);
			global[bone] = skeleton->get_bone_global_pose(bone);
		}
	}
};

struct Rig {
	Node3D *root = memnew(Node3D);
	Skeleton3D *skeleton = memnew(Skeleton3D);
	Node3D *target = memnew(Node3D);
	Node3D *pole = memnew(Node3D);
	Ref<FinalPoseRecorder> recorder;

	explicit Rig(bool p_chain) {
		root->set_transform(Transform3D(Basis(Quaternion(Vector3(0, 1, 0), 0.3)).scaled_local(Vector3(1.25, 1.25, 1.25)), Vector3(3, 2, -1)));
		root->add_child(skeleton);
		root->add_child(target);
		root->add_child(pole);
		skeleton->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
		const int chain_parents[] = { -1, 0, 1, 2, 3 };
		const int copy_parents[] = { -1, 0, 0, 1, 2 };
		const Vector3 positions[] = { Vector3(0.1, 0.2, 0), Vector3(0.4, 0, 0), Vector3(0.6, 0.1, 0), Vector3(0.2, 0, 0), Vector3(0.8, 0, 0) };
		for (int bone = 0; bone < 5; bone++) {
			skeleton->add_bone("Bone" + itos(bone));
			skeleton->set_bone_parent(bone, p_chain ? chain_parents[bone] : copy_parents[bone]);
			skeleton->set_bone_rest(bone, Transform3D(Basis(Quaternion(Vector3(0, 0, 1), 0.03 * bone)), positions[bone]));
			skeleton->set_bone_pose_position(bone, positions[bone]);
			skeleton->set_bone_pose_rotation(bone, Quaternion(Vector3(0, 1, 0), 0.04 * bone));
		}
		target->set_transform(Transform3D(Basis(Quaternion(Vector3(0, 0, 1), 0.4)).scaled_local(Vector3(1.2, 0.8, 1.1)), Vector3(1.3, 0.7, 0.25)));
		pole->set_position(Vector3(0.4, 1.4, 0.7));
		SceneTree::get_singleton()->get_root()->add_child(root);
		recorder.instantiate();
		recorder->skeleton = skeleton;
		skeleton->connect(SNAME("skeleton_updated"), callable_mp(recorder.ptr(), &FinalPoseRecorder::record));
	}

	void advance() {
		skeleton->advance(0);
		skeleton->notification(Skeleton3D::NOTIFICATION_UPDATE_SKELETON);
		REQUIRE(recorder->notifications > 0);
	}

	~Rig() { memdelete(root); }
};

static void evaluate_on_worker(const Ref<SkeletonAnimationPose> &p_pose) {
#ifdef THREADS_ENABLED
	Thread thread;
	REQUIRE(thread.start([](void *p_data) { static_cast<SkeletonAnimationPose *>(p_data)->evaluate(); }, p_pose.ptr()) != Thread::ID());
	thread.wait_to_finish();
#else
	p_pose->evaluate();
#endif
}

static void compare_final(const Rig &p_original, const Ref<SkeletonAnimationPose> &p_pose) {
	for (int bone = 0; bone < p_original.skeleton->get_bone_count(); bone++) {
		INFO("bone ", bone);
		CHECK(p_pose->get_local_pose(bone).is_equal_approx(p_original.recorder->local[bone]));
		CHECK(p_pose->get_global_pose(bone).is_equal_approx(p_original.recorder->global[bone]));
	}
}

TEST_CASE("[SceneTree][CZAnimation][CopyTransformModifier3D] Detached settings masks and outer influence match synchronous publication") {
	for (bool node_reference : { false, true }) {
		for (bool relative : { false, true }) {
			for (bool additive : { false, true }) {
				for (real_t influence : { real_t(0), real_t(0.4), real_t(1) }) {
					INFO("node ", node_reference, " relative ", relative, " additive ", additive, " influence ", influence);
					Rig original(false);
					Rig detached(false);
					for (Rig *rig : { &original, &detached }) {
						CopyTransformModifier3D *copy = memnew(CopyTransformModifier3D);
						copy->set_setting_count(2);
						copy->set_influence(influence);
						rig->skeleton->add_child(copy);
						for (int setting = 0; setting < 2; setting++) {
							copy->set_apply_bone(setting, setting ? 3 : 1);
							copy->set_reference_type(setting, node_reference ? BoneConstraint3D::REFERENCE_TYPE_NODE : BoneConstraint3D::REFERENCE_TYPE_BONE);
							copy->set_reference_node(setting, copy->get_path_to(rig->target));
							copy->set_reference_bone(setting, setting ? 1 : 2);
							copy->set_amount(setting, setting ? 0.5 : 0.65);
							copy->set_global(setting, true);
							copy->set_relative(setting, relative);
							copy->set_additive(setting, additive);
							copy->set_axis_y_enabled(setting, false);
							copy->set_axis_z_inverted(setting, true);
						}
					}
					original.advance();
					Ref<SkeletonAnimationPose> pose;
					pose.instantiate();
					REQUIRE(pose->capture(detached.skeleton));
					// Captured node inputs remain owned values until publication.
					detached.target->set_position(Vector3(-10, -20, 10));
					evaluate_on_worker(pose);
					compare_final(original, pose);
					REQUIRE(pose->publish());
					for (int bone = 0; bone < 5; bone++) {
						CHECK(detached.recorder->global[bone].is_equal_approx(original.recorder->global[bone]));
						CHECK(detached.skeleton->get_bone_pose(bone).is_equal_approx(original.skeleton->get_bone_pose(bone)));
					}
				}
			}
		}
	}
}

TEST_CASE("[SceneTree][CZAnimation][TwoBoneIK3D] Detached mutable and fixed chains retain interposed bones and frame history") {
	for (bool mutable_axes : { false, true }) {
		for (bool virtual_end : { false, true }) {
			for (real_t influence : { real_t(0), real_t(0.35), real_t(1) }) {
				INFO("mutable ", mutable_axes, " virtual end ", virtual_end, " influence ", influence);
				Rig original(true);
				Rig detached(true);
				for (Rig *rig : { &original, &detached }) {
					TwoBoneIK3D *ik = memnew(TwoBoneIK3D);
					ik->set_setting_count(1);
					ik->set_influence(influence);
					ik->set_mutable_bone_axes(mutable_axes);
					rig->skeleton->add_child(ik);
					ik->set_root_bone(0, 0);
					ik->set_middle_bone(0, 2);
					ik->set_end_bone(0, 4);
					ik->set_use_virtual_end(0, virtual_end);
					ik->set_extend_end_bone(0, virtual_end);
					ik->set_end_bone_direction(0, SkeletonModifier3D::BONE_DIRECTION_PLUS_X);
					ik->set_end_bone_length(0, 1);
					ik->set_target_node(0, ik->get_path_to(rig->target));
					ik->set_pole_node(0, ik->get_path_to(rig->pole));
					ik->set_pole_direction(0, SkeletonModifier3D::SECONDARY_DIRECTION_NONE);
				}
				Ref<SkeletonAnimationPose> pose;
				pose.instantiate();
				for (int frame = 0; frame < 2; frame++) {
					const Vector3 target(1.3 + 0.1 * frame, 0.7 - 0.15 * frame, 0.25);
					original.target->set_position(target);
					detached.target->set_position(target);
					original.advance();
					REQUIRE(pose->capture(detached.skeleton));
					evaluate_on_worker(pose);
					compare_final(original, pose);
					REQUIRE(pose->publish());
					for (int bone = 0; bone < 5; bone++) {
						CHECK(detached.recorder->global[bone].is_equal_approx(original.recorder->global[bone]));
						CHECK(detached.skeleton->get_bone_pose(bone).is_equal_approx(original.skeleton->get_bone_pose(bone)));
					}
				}
			}
		}
	}
}

} // namespace TestCZModifierSolvers
