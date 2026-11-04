/**************************************************************************/
/*  test_animation_runtime.h                                              */
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
#include "scene/animation/animation_blend_tree.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "tests/test_macros.h"

#include "modules/cz_animation/animation_batch_processor.h"

namespace TestCZAnimationRuntime {

class AppliedCounter : public RefCounted {
	GDCLASS(AppliedCounter, RefCounted);

protected:
	static void _bind_methods() {}

public:
	int count = 0;
	void on_applied() { count++; }
};

struct Rig {
	Node3D *root = nullptr;
	Skeleton3D *skeleton = nullptr;
	AnimationPlayer *player = nullptr;
	AnimationTree *tree = nullptr;
	Ref<AnimationNodeAnimation> animation_node;
	Ref<Animation> animation;
	Ref<AppliedCounter> observer;
	Ref<SkeletonAnimationPose> pose;
	Ref<AnimationBatchProcessor> processor;
	int64_t handle = 0;

	explicit Rig(const Ref<AnimationBatchProcessor> &p_processor = Ref<AnimationBatchProcessor>()) {
		root = memnew(Node3D);
		skeleton = memnew(Skeleton3D);
		skeleton->set_name("Skeleton3D");
		skeleton->add_bone("Root");
		skeleton->add_bone("Finger");
		skeleton->set_bone_parent(1, 0);
		root->add_child(skeleton);
		player = memnew(AnimationPlayer);
		player->set_name("Player");
		player->set_root_node(NodePath(".."));
		root->add_child(player);
		Ref<AnimationLibrary> library;
		library.instantiate();
		animation.instantiate();
		animation->set_length(1);
		animation->set_loop_mode(Animation::LOOP_LINEAR);
		const int track = animation->add_track(Animation::TYPE_POSITION_3D);
		animation->track_set_path(track, NodePath("Skeleton3D:Root"));
		animation->position_track_insert_key(track, 0, Vector3());
		animation->position_track_insert_key(track, 1, Vector3(10, 0, 0));
		library->add_animation("Move", animation);
		Ref<Animation> still;
		still.instantiate();
		const int still_track = still->add_track(Animation::TYPE_POSITION_3D);
		still->track_set_path(still_track, NodePath("Skeleton3D:Root"));
		still->position_track_insert_key(still_track, 0, Vector3(4, 0, 0));
		library->add_animation("Still", still);
		player->add_animation_library(StringName(), library);
		tree = memnew(AnimationTree);
		tree->set_name("Tree");
		tree->set_callback_mode_process(AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
		root->add_child(tree);
		tree->set_animation_player(NodePath("../Player"));
		Ref<AnimationNodeBlendTree> graph;
		graph.instantiate();
		animation_node.instantiate();
		animation_node->set_animation("Move");
		graph->add_node("Clip", animation_node);
		graph->connect_node("output", 0, "Clip");
		tree->set_root_animation_node(graph);
		SceneTree::get_singleton()->get_root()->add_child(root);
		tree->advance(0);
		observer.instantiate();
		tree->connect(SNAME("mixer_applied"), callable_mp(observer.ptr(), &AppliedCounter::on_applied));
		pose.instantiate();
		if (p_processor.is_valid()) {
			processor = p_processor;
		} else {
			processor.instantiate();
		}
		REQUIRE(pose->capture(skeleton));
		handle = processor->register_tree(tree, { pose }, {});
		REQUIRE(handle != 0);
	}

	void submit(double p_delta = 0.1, bool p_full_bones = false) {
		REQUIRE(processor->queue_update_with_options(handle, p_delta, true, true, true, 0, 0, p_full_bones));
		processor->submit(8, 1);
	}

	~Rig() {
		processor->unregister_tree(handle);
		processor->complete_and_publish();
		memdelete(root);
	}
};

class ReentryAction : public RefCounted {
	GDCLASS(ReentryAction, RefCounted);

protected:
	static void _bind_methods() {}

public:
	Ref<AnimationBatchProcessor> processor;
	LocalVector<int64_t> handles;
	Ref<SkeletonAnimationPose> capture_pose;
	Ref<SkeletonAnimationPose> *drop_pose = nullptr;
	Ref<SkinReference> *drop_skin = nullptr;
	Skeleton3D *capture_target = nullptr;
	bool nested_publish = false;
	bool captured = false;
	int calls = 0;

	void submit() {
		calls++;
		for (int64_t handle : handles) {
			REQUIRE(processor->queue_update(handle, 0.1));
		}
		processor->submit(8, 2);
		if (nested_publish) {
			processor->complete_and_publish();
		}
	}
	void capture() { captured = capture_pose->capture(capture_target); }
	void drop() { drop_pose->unref(); }
	void release_skin() { drop_skin->unref(); }
};

TEST_CASE("[SceneTree][CZAnimation] Complete waits and publication is explicit and idempotent") {
	Rig rig;
	rig.submit();
	CHECK(rig.pose->get_local_pose(0).origin.is_equal_approx(Vector3(1, 0, 0)));
	CHECK(rig.observer->count == 0); // A pose query joins without publishing.
	rig.processor->complete();
	CHECK(rig.observer->count == 0);
	rig.processor->publish(rig.handle);
	CHECK(rig.observer->count == 1);
	rig.processor->publish(rig.handle);
	rig.processor->complete_and_publish();
	CHECK(rig.observer->count == 1);
	CHECK(rig.processor->was_evaluated(rig.handle));
}

TEST_CASE("[SceneTree][CZAnimation] Shared clip and graph resource mutations finish readers before writing") {
	Rig rig;
	rig.submit();
	rig.animation->position_track_insert_key(0, 0.5, Vector3(8, 0, 0));
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(rig.observer->count == 1);
	rig.submit();
	rig.animation_node->set_animation("Still");
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(rig.observer->count == 2);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(4, 0, 0)));
}

TEST_CASE("[SceneTree][CZAnimation] Bone LOD and ForceFullBones do not change the configured distance level") {
	Rig rig;
	rig.pose->configure_bone_lod({ "Finger" }, { 1 });
	rig.pose->set_bone_lod(2);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.pose->get_active_bone_count() == 1);
	CHECK(rig.pose->get_bone_lod() == 2);
	rig.submit(0, true);
	rig.processor->complete_and_publish();
	CHECK(rig.pose->get_active_bone_count() == 2);
	CHECK(rig.pose->get_bone_lod() == 2);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.pose->get_active_bone_count() == 1);
}

TEST_CASE("[SceneTree][CZAnimation] Unregistration discards a pending result and restores ownership") {
	Rig rig;
	rig.submit();
	rig.processor->unregister_tree(rig.handle);
	rig.processor->complete_and_publish();
	CHECK(rig.observer->count == 0);
	CHECK_FALSE(rig.processor->was_evaluated(rig.handle));
	CHECK(rig.tree->get_callback_mode_process() == AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
}

TEST_CASE("[SceneTree][CZAnimation] Pose mutation finishes a pending frame before changing its capture") {
	Rig rig;
	rig.submit();
	rig.pose->release();
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(rig.observer->count == 1);
	CHECK(rig.processor->get_statistics()["publish_entries"] == Variant(1));
	rig.submit();
	CHECK(rig.pose->capture(rig.skeleton));
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(rig.observer->count == 2);
}

TEST_CASE("[SceneTree][CZAnimation] Registrations reject shared mutable poses and skeletons") {
	Rig rig;
	AnimationTree *other = memnew(AnimationTree);
	rig.root->add_child(other);
	ERR_PRINT_OFF;
	CHECK(rig.processor->register_tree(other, { rig.pose }, {}) == 0);
	ERR_PRINT_ON;
	Ref<SkeletonAnimationPose> second_pose;
	second_pose.instantiate();
	CHECK(second_pose->capture(rig.skeleton));
	ERR_PRINT_OFF;
	CHECK(rig.processor->register_tree(other, { second_pose }, {}) == 0);
	ERR_PRINT_ON;
	second_pose->release();
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->was_evaluated(rig.handle));
	memdelete(other);
}

TEST_CASE("[SceneTree][CZAnimation] Multiple standalone captures share callback ownership until the last release") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	skeleton->add_bone("Root");
	skeleton->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_PHYSICS);
	SceneTree::get_singleton()->get_root()->add_child(skeleton);
	Ref<SkeletonAnimationPose> first;
	Ref<SkeletonAnimationPose> second;
	first.instantiate();
	second.instantiate();
	REQUIRE(first->capture(skeleton));
	REQUIRE(second->capture(skeleton));
	first->release();
	CHECK(skeleton->get_modifier_callback_mode_process() == Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
	second->release();
	CHECK(skeleton->get_modifier_callback_mode_process() == Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_PHYSICS);
	memdelete(skeleton);
}

#ifdef THREADS_ENABLED
TEST_CASE("[SceneTree][CZAnimation] Standalone pose destruction queues scene cleanup from a worker") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	skeleton->add_bone("Root");
	skeleton->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_PHYSICS);
	SceneTree::get_singleton()->get_root()->add_child(skeleton);
	struct CleanupJob {
		Ref<SkeletonAnimationPose> pose;
		bool worker = false;
	} job;
	job.pose.instantiate();
	REQUIRE(job.pose->capture(skeleton));
	Thread thread;
	REQUIRE(thread.start([](void *p_data) {
		CleanupJob *job = static_cast<CleanupJob *>(p_data);
		job->worker = !Thread::is_main_thread();
		job->pose.unref();
	},
					&job) != Thread::ID());
	thread.wait_to_finish();
	CHECK(job.worker);
	CHECK(skeleton->get_modifier_callback_mode_process() == Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
	AnimationBatchProcessor::finish_pending_frames();
	CHECK(skeleton->get_modifier_callback_mode_process() == Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_PHYSICS);
	memdelete(skeleton);
}
#endif

TEST_CASE("[SceneTree][CZAnimation] Resource synchronization joins callback generations without unbounded publication") {
	Rig first;
	Ref<AnimationBatchProcessor> second_processor;
	second_processor.instantiate();
	LocalVector<Rig *> second;
	Ref<ReentryAction> submit_second;
	Ref<ReentryAction> submit_first;
	submit_second.instantiate();
	submit_first.instantiate();
	submit_second->processor = second_processor;
	submit_first->processor = first.processor;
	submit_first->handles.push_back(first.handle);
	for (int i = 0; i < 16; i++) {
		Rig *rig = memnew(Rig(second_processor));
		Ref<AnimationLibrary> library = rig->player->get_animation_library(StringName());
		library->remove_animation("Move");
		library->add_animation("Move", first.animation);
		second.push_back(rig);
		submit_second->handles.push_back(rig->handle);
	}
	const Callable second_callback = callable_mp(submit_second.ptr(), &ReentryAction::submit);
	const Callable first_callback = callable_mp(submit_first.ptr(), &ReentryAction::submit);
	first.tree->connect(SNAME("mixer_applied"), second_callback);
	second[0]->tree->connect(SNAME("mixer_applied"), first_callback);
	first.submit();
	// Use the same barrier as a resource writer. Check its callback generations
	// before emitting the subsequent resource's separate changed notifications.
	CZAnimationBridge::synchronize();
	CHECK(submit_second->calls == 1);
	CHECK(submit_first->calls == 0);
	CHECK(second_processor->is_pending());
	CHECK(second_processor->get_statistics().has("sample_blend_us"));
	CZAnimationBridge::synchronize();
	CHECK(submit_first->calls == 1);
	CHECK(submit_second->calls == 1);
	CHECK(first.processor->is_pending());
	CHECK(first.processor->get_statistics().has("sample_blend_us"));
	first.tree->disconnect(SNAME("mixer_applied"), second_callback);
	second[0]->tree->disconnect(SNAME("mixer_applied"), first_callback);
	first.animation->position_track_insert_key(0, 0.5, Vector3(8, 0, 0));
	for (Rig *rig : second) {
		memdelete(rig);
	}
}

TEST_CASE("[SceneTree][CZAnimation] Nested publication restores the outer animation context") {
	Rig first;
	Rig second;
	Ref<ReentryAction> action;
	action.instantiate();
	action->processor = second.processor;
	action->handles.push_back(second.handle);
	action->nested_publish = true;
	first.skeleton->connect(SNAME("skeleton_updated"), callable_mp(action.ptr(), &ReentryAction::submit));
	first.submit();
	first.processor->complete_and_publish();
	CHECK(action->calls == 1);
	CHECK(first.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(1, 0, 0)));
	CHECK(second.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(1, 0, 0)));
}

TEST_CASE("[SceneTree][CZAnimation] Capture reentry and configuration callouts retain their correct owners") {
	Skeleton3D *first = memnew(Skeleton3D);
	Skeleton3D *second = memnew(Skeleton3D);
	for (Skeleton3D *skeleton : { first, second }) {
		skeleton->add_bone("Root");
		skeleton->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_PHYSICS);
		SceneTree::get_singleton()->get_root()->add_child(skeleton);
	}
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	Ref<ReentryAction> capture;
	capture.instantiate();
	capture->capture_pose = pose;
	capture->capture_target = second;
	first->connect(SNAME("bone_list_changed"), callable_mp(capture.ptr(), &ReentryAction::capture));
	// Entering SceneTree may already have resolved the initial bone order.
	// Make the hierarchy dirty after connecting the callback so Capture emits it.
	first->set_bone_parent(0, -1);
	CHECK_FALSE(pose->capture(first));
	CHECK(capture->captured);
	CHECK(pose->get_skeleton_id() == second->get_instance_id());
	pose->release();
	CHECK(first->get_modifier_callback_mode_process() == Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_PHYSICS);
	CHECK(second->get_modifier_callback_mode_process() == Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_PHYSICS);
	memdelete(first);
	memdelete(second);

	Rig rig;
	Ref<SkeletonAnimationPose> target;
	target.instantiate();
	const ObjectID target_id = target->get_instance_id();
	Ref<ReentryAction> drop;
	drop.instantiate();
	drop->drop_pose = &target;
	rig.tree->connect(SNAME("mixer_applied"), callable_mp(drop.ptr(), &ReentryAction::drop));
	rig.submit();
	SkeletonAnimationPose *raw_target = target.ptr();
	raw_target->configure_aim(0, Dictionary());
	CHECK(target.is_null());
	CHECK(ObjectDB::get_instance(target_id) == nullptr);
}

TEST_CASE("[SceneTree][CZAnimation] Pose sources reject cycles and unordered cross-entry dependencies") {
	Ref<SkeletonAnimationPose> first;
	Ref<SkeletonAnimationPose> second;
	first.instantiate();
	second.instantiate();
	const ObjectID first_id = first->get_instance_id();
	const ObjectID second_id = second->get_instance_id();
	first->configure_copy_source(second, 0, 0);
	ERR_PRINT_OFF;
	second->configure_attachment(first, 0, Transform3D(), Transform3D(), false);
	ERR_PRINT_ON;
	first.unref();
	second.unref();
	CHECK(ObjectDB::get_instance(first_id) == nullptr);
	CHECK(ObjectDB::get_instance(second_id) == nullptr);
	Rig source;
	Rig consumer;
	consumer.pose->configure_copy_source(source.pose, 0, 0);
	consumer.submit();
	consumer.processor->complete_and_publish();
	CHECK_FALSE(consumer.processor->was_evaluated(consumer.handle));
	CHECK(consumer.processor->get_fallback_reason(consumer.handle).contains("not ordered"));
}

TEST_CASE("[SceneTree][CZAnimation] Publication rejects retargeted captures and removed skin bindings") {
	Skeleton3D *first = memnew(Skeleton3D);
	Skeleton3D *second = memnew(Skeleton3D);
	for (Skeleton3D *skeleton : { first, second }) {
		skeleton->add_bone("Root");
		SceneTree::get_singleton()->get_root()->add_child(skeleton);
	}
	second->add_bone("Extra");
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	REQUIRE(pose->capture(first));
	pose->evaluate();
	Ref<ReentryAction> action;
	action.instantiate();
	action->capture_pose = pose;
	action->capture_target = second;
	const Callable retarget = callable_mp(action.ptr(), &ReentryAction::capture);
	first->connect(SNAME("skeleton_updated"), retarget);
	CHECK_FALSE(pose->publish());
	CHECK(action->captured);
	CHECK(pose->get_skeleton_id() == second->get_instance_id());
	first->disconnect(SNAME("skeleton_updated"), retarget);
	pose->release();

	Ref<Skin> skin;
	skin.instantiate();
	skin->add_bind(0, Transform3D());
	Ref<SkinReference> reference = first->register_skin(skin);
	action->drop_skin = &reference;
	REQUIRE(pose->capture(first));
	pose->evaluate();
	first->connect(SNAME("skeleton_updated"), callable_mp(action.ptr(), &ReentryAction::release_skin));
	CHECK_FALSE(pose->publish());
	CHECK(reference.is_null());
	CHECK(pose->capture_current());
	pose->release();
	memdelete(first);
	memdelete(second);
}

TEST_CASE("[SceneTree][CZAnimation] Animated captures retain SoA storage and refresh only changed native inputs") {
	Rig rig;
	rig.tree->set_deterministic(false);
	rig.submit();
	rig.processor->complete_and_publish();
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->get_statistics()["captured_bones"] == Variant(0));
	// A channel absent from all clips must retain explicit native input changes.
	rig.skeleton->set_bone_pose_position(1, Vector3(0, 3, 0));
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->get_statistics()["captured_bones"] == Variant(2));
	CHECK(rig.pose->get_local_pose(1).origin.is_equal_approx(Vector3(0, 3, 0)));
	const Vector3 held_root = rig.skeleton->get_bone_pose_position(0);
	rig.animation->track_set_enabled(0, false);
	REQUIRE(rig.processor->queue_update_with_options(rig.handle, 0.1, false, true, false, 0.4, 0.1));
	rig.processor->submit(8, 1);
	rig.processor->complete_and_publish();
	CHECK(rig.processor->get_statistics()["captured_bones"] == Variant(2));
	CHECK(rig.processor->get_statistics()["samples"] == Variant(1));
	CHECK(rig.pose->get_local_pose(0).origin.is_equal_approx(held_root));
	rig.animation->track_set_enabled(0, true);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.pose->get_local_pose(0).origin.is_equal_approx(Vector3(5, 0, 0)));
	CHECK(rig.pose->get_local_pose(1).origin.is_equal_approx(Vector3(0, 3, 0)));
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->get_statistics()["captured_bones"] == Variant(0));
}

TEST_CASE("[SceneTree][CZAnimation] Zero-weight transform caches preserve the published base only when non-deterministic") {
	for (bool deterministic : { false, true }) {
		Rig original;
		Rig batched;
		original.processor->unregister_tree(original.handle);
		for (Rig *rig : { &original, &batched }) {
			rig->tree->set_deterministic(deterministic);
			Ref<Animation> empty;
			empty.instantiate();
			rig->player->get_animation_library(StringName())->add_animation("Empty", empty);
		}
		BoneRotationOffsetModifier3D *modifier = memnew(BoneRotationOffsetModifier3D);
		modifier->set_bone_names({ "Root" });
		modifier->set_rotation_offset(Quaternion(Vector3(0, 1, 0), 0.4));
		batched.skeleton->add_child(modifier);
		original.tree->advance(0.2);
		batched.submit(0.2);
		batched.processor->complete_and_publish();
		for (Rig *rig : { &original, &batched }) {
			rig->animation_node->set_animation("Empty");
		}
		original.tree->advance(0.1);
		batched.submit(0.1);
		batched.processor->complete_and_publish();
		CHECK(batched.processor->was_evaluated(batched.handle));
		CHECK(batched.skeleton->get_bone_pose(0).is_equal_approx(original.skeleton->get_bone_pose(0)));
		CHECK(batched.pose->get_base_local_pose(0).is_equal_approx(original.skeleton->get_bone_pose(0)));
		CHECK(batched.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(deterministic ? 0 : 2, 0, 0)));
		CHECK(batched.skeleton->get_bone_pose_rotation(0).is_equal_approx(Quaternion()));
	}
}

#ifndef DISABLE_DEPRECATED
TEST_CASE("[SceneTree][CZAnimation] Live deprecated global overrides invalidate the detached binding and recover after clearing") {
	for (bool persistent : { false, true }) {
		Rig rig;
		rig.submit();
		rig.processor->complete_and_publish();
		rig.skeleton->set_bone_global_pose_override(0, Transform3D(Basis(), Vector3(9, 0, 0)), 1, persistent);
		rig.submit();
		rig.processor->complete_and_publish();
		CHECK_FALSE(rig.processor->was_evaluated(rig.handle));
		CHECK(rig.processor->get_fallback_reason(rig.handle).contains("global pose override"));
		rig.skeleton->clear_bones_global_pose_override();
		rig.submit();
		rig.processor->complete_and_publish();
		CHECK(rig.processor->was_evaluated(rig.handle));
	}
}
#endif

} // namespace TestCZAnimationRuntime
