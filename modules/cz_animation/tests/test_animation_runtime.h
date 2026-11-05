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

#include "core/error/error_macros.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/animation/animation_blend_space_1d.h"
#include "scene/animation/animation_blend_space_2d.h"
#include "scene/animation/animation_blend_tree.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/audio/audio_stream_wav.h"
#include "scene/resources/mesh.h"
#include "servers/audio/audio_driver_dummy.h"
#include "servers/audio/audio_server.h"
#include "tests/test_macros.h"

#include "modules/cz_animation/animation_batch_processor.h"
#include "modules/cz_animation/animation_hooks.h"

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

	explicit Rig(const Ref<AnimationBatchProcessor> &p_processor = Ref<AnimationBatchProcessor>(), const PackedStringArray &p_safe_methods = PackedStringArray()) {
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
		handle = processor->register_tree(tree, { pose }, p_safe_methods);
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

static Ref<Animation> make_method_animation(const StringName &p_method) {
	Ref<Animation> animation;
	animation.instantiate();
	const int track = animation->add_track(Animation::TYPE_METHOD);
	animation->track_set_path(track, NodePath("."));
	Dictionary key;
	key["method"] = p_method;
	key["args"] = Array{ 0.125 };
	animation->track_insert_key(track, 0.5, key);
	return animation;
}

static int64_t method_statistic(const Ref<AnimationBatchProcessor> &p_processor, const StringName &p_name) {
	return int64_t(p_processor->get_statistics().get(p_name, int64_t(-1)));
}

struct PhaseDiagnosticsEnvironment {
	bool existed = OS::get_singleton()->has_environment("CZ_ANIM_PHASE_DIAGNOSTICS");
	String previous = OS::get_singleton()->get_environment("CZ_ANIM_PHASE_DIAGNOSTICS");

	explicit PhaseDiagnosticsEnvironment(bool p_enabled) { set_enabled(p_enabled); }
	void set_enabled(bool p_enabled) { OS::get_singleton()->set_environment("CZ_ANIM_PHASE_DIAGNOSTICS", p_enabled ? "1" : "0"); }
	~PhaseDiagnosticsEnvironment() {
		if (existed) {
			OS::get_singleton()->set_environment("CZ_ANIM_PHASE_DIAGNOSTICS", previous);
		} else {
			OS::get_singleton()->unset_environment("CZ_ANIM_PHASE_DIAGNOSTICS");
		}
	}
};

struct PoseFastpathsEnvironment {
	bool existed = OS::get_singleton()->has_environment("CZ_ANIM_DISABLE_POSE_FASTPATHS");
	String previous = OS::get_singleton()->get_environment("CZ_ANIM_DISABLE_POSE_FASTPATHS");

	explicit PoseFastpathsEnvironment(bool p_enabled) { set_enabled(p_enabled); }
	void set_enabled(bool p_enabled) { OS::get_singleton()->set_environment("CZ_ANIM_DISABLE_POSE_FASTPATHS", p_enabled ? "0" : "1"); }
	~PoseFastpathsEnvironment() {
		if (existed) {
			OS::get_singleton()->set_environment("CZ_ANIM_DISABLE_POSE_FASTPATHS", previous);
		} else {
			OS::get_singleton()->unset_environment("CZ_ANIM_DISABLE_POSE_FASTPATHS");
		}
	}
};

struct ProcessorRefProbe {
	AnimationBatchProcessor *owner;
	int increments = 0;
	int decrements = 0;

	static GDExtensionBool on_reference(void *, void *p_binding, GDExtensionBool p_reference) {
		ProcessorRefProbe *self = static_cast<ProcessorRefProbe *>(p_binding);
		if (p_reference) {
			self->increments++;
		} else {
			self->decrements++;
		}
		return true;
	}

	explicit ProcessorRefProbe(AnimationBatchProcessor *p_owner) :
			owner(p_owner) {
		GDExtensionInstanceBindingCallbacks callbacks = {};
		callbacks.reference_callback = &ProcessorRefProbe::on_reference;
		owner->set_instance_binding(this, this, &callbacks);
	}
	~ProcessorRefProbe() { owner->free_instance_binding(this); }
	void reset() { increments = decrements = 0; }
};

TEST_CASE("[SceneTree][CZAnimation] Idle synchronization avoids reference transitions and pending queries do not publish") {
	Rig rig;
	REQUIRE(rig.processor->get_reference_count() == 1);
	// Public native binding callbacks observe real 1->2->1 transitions. This
	// does not substitute a fake CLR host or assert managed allocation timing.
	ProcessorRefProbe probe(rig.processor.ptr());
	{
		Ref<AnimationBatchProcessor> calibration = rig.processor;
	}
	REQUIRE(probe.increments == 1);
	REQUIRE(probe.decrements == 1);
	const auto check_idle = [&]() {
		probe.reset();
		for (int i = 0; i < 8; i++) {
			CZAnimationBridge::synchronize(rig.tree);
			CZAnimationBridge::complete_pose(rig.pose.ptr());
		}
		CHECK(probe.increments == 0);
		CHECK(probe.decrements == 0);
		CHECK(rig.processor->get_reference_count() == 1);
	};
	check_idle();
	rig.submit();
	CHECK(rig.processor->is_pending());
	CHECK(rig.pose->get_local_pose(0).origin.is_equal_approx(Vector3(1, 0, 0)));
	CHECK(rig.observer->count == 0);
	CZAnimationBridge::synchronize(rig.tree);
	CHECK(rig.observer->count == 1);
	rig.processor->complete_and_publish();
	CHECK(rig.observer->count == 1);
	CHECK_FALSE(rig.processor->is_pending());
	check_idle();
}

struct AuxiliaryTracks {
	Node3D *target = nullptr;
	MeshInstance3D *mesh_instance = nullptr;

	explicit AuxiliaryTracks(Rig &p_rig) {
		target = memnew(Node3D);
		target->set_name("Target");
		p_rig.root->add_child(target);
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_name("Mesh");
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_blend_shape("Smile");
		Vector<Vector3> vertices{ Vector3(), Vector3(1, 0, 0), Vector3(0, 1, 0) };
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vertices;
		TypedArray<Array> blend_shapes;
		blend_shapes.push_back(arrays);
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, blend_shapes);
		mesh_instance->set_mesh(mesh);
		p_rig.root->add_child(mesh_instance);
		int track = p_rig.animation->add_track(Animation::TYPE_VALUE);
		p_rig.animation->track_set_path(track, NodePath("Target:position"));
		p_rig.animation->track_insert_key(track, 0, Vector3());
		p_rig.animation->track_insert_key(track, 1, Vector3(0, 2, 0));
		track = p_rig.animation->add_track(Animation::TYPE_BLEND_SHAPE);
		p_rig.animation->track_set_path(track, NodePath("Mesh:Smile"));
		p_rig.animation->blend_shape_track_insert_key(track, 0, 0);
		p_rig.animation->blend_shape_track_insert_key(track, 1, 1);
		track = p_rig.animation->add_track(Animation::TYPE_METHOD);
		p_rig.animation->track_set_path(track, NodePath("Target"));
		Dictionary key;
		key["method"] = StringName("rotate_y");
		key["args"] = Array{ 0.125 };
		for (double time : { 0.0, 0.2, 0.8 }) {
			p_rig.animation->track_insert_key(track, time, key);
		}
	}
};

static void check_phase_diagnostics(const Dictionary &p_statistics) {
	for (const char *key : { "submit_total_us", "submit_inline_us", "evaluate_total_us", "evaluate_graph_us", "evaluate_weights_us", "evaluate_pose_us", "evaluate_aux_blend_us", "evaluate_calls", "evaluate_first_key_calls" }) {
		REQUIRE(p_statistics.has(key));
		CHECK(int64_t(p_statistics[key]) >= 0);
	}
	const int64_t total = p_statistics["evaluate_total_us"];
	const int64_t children = int64_t(p_statistics["evaluate_graph_us"]) + int64_t(p_statistics["evaluate_weights_us"]) + int64_t(p_statistics["evaluate_pose_us"]) + int64_t(p_statistics["evaluate_aux_blend_us"]);
	CHECK(total >= children);
	CHECK(int64_t(p_statistics["submit_total_us"]) >= int64_t(p_statistics["prepare_us"]) + int64_t(p_statistics["submit_inline_us"]));
	if (int64_t(p_statistics["workers"]) == 1 && int64_t(p_statistics["recomputed"]) == 0) {
		CHECK(int64_t(p_statistics["submit_inline_us"]) >= total);
	}
}

TEST_CASE("[SceneTree][CZAnimation] Phase diagnostics latch at construction and omit disabled timings") {
	PhaseDiagnosticsEnvironment environment(false);
	Ref<AnimationBatchProcessor> disabled;
	disabled.instantiate();
	environment.set_enabled(true);
	Ref<AnimationBatchProcessor> enabled;
	enabled.instantiate();
	environment.set_enabled(false);
	CHECK_FALSE(bool(disabled->get_statistics()["phase_diagnostics_enabled"]));
	CHECK(bool(enabled->get_statistics()["phase_diagnostics_enabled"]));
	CHECK_FALSE(enabled->get_statistics().has("evaluate_total_us"));
	Rig disabled_rig(disabled);
	Rig enabled_rig(enabled);
	disabled_rig.submit();
	disabled->complete_and_publish();
	const Dictionary disabled_statistics = disabled->get_statistics();
	CHECK_FALSE(bool(disabled_statistics["phase_diagnostics_enabled"]));
	for (const char *key : { "submit_total_us", "submit_inline_us", "evaluate_total_us", "evaluate_graph_us", "evaluate_weights_us", "evaluate_pose_us", "evaluate_aux_blend_us", "evaluate_calls", "evaluate_first_key_calls", "evaluate_instance_track_skips", "publish_instance_track_skips" }) {
		CHECK_FALSE(disabled_statistics.has(key));
	}
	enabled_rig.submit();
	CHECK_FALSE(enabled->get_statistics().has("evaluate_total_us"));
	CHECK_FALSE(enabled->get_statistics().has("evaluate_instance_track_skips"));
	CHECK_FALSE(enabled->get_statistics().has("publish_instance_track_skips"));
	enabled->complete();
	CHECK_FALSE(enabled->get_statistics().has("evaluate_total_us"));
	CHECK_FALSE(enabled->get_statistics().has("evaluate_instance_track_skips"));
	CHECK_FALSE(enabled->get_statistics().has("publish_instance_track_skips"));
	CHECK(enabled_rig.observer->count == 0);
	enabled->complete_and_publish();
	const Dictionary statistics = enabled->get_statistics();
	check_phase_diagnostics(statistics);
	CHECK(int64_t(statistics["evaluate_calls"]) == 1);
	CHECK(int64_t(statistics["evaluate_first_key_calls"]) == 0);
	CHECK(enabled_rig.observer->count == 1);
	CHECK(enabled_rig.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(1, 0, 0)));
}

TEST_CASE("[SceneTree][CZAnimation] Phase diagnostics reset per submit and retain publication recomputation") {
	PhaseDiagnosticsEnvironment environment(true);
	Rig rig;
	rig.processor->unregister_tree(rig.handle);
	rig.player->set_callback_mode_process(AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
	rig.player->play("Move");
	REQUIRE(rig.pose->capture(rig.skeleton));
	rig.handle = rig.processor->register_player(rig.player, { rig.pose }, PackedStringArray());
	REQUIRE(rig.handle != 0);
	rig.submit();
	// The real Player dependency path marks the pending output dirty for publish.
	rig.processor->finish_for_dependency(rig.player);
	rig.processor->complete_and_publish();
	const Dictionary recomputed_statistics = rig.processor->get_statistics();
	check_phase_diagnostics(recomputed_statistics);
	CHECK(int64_t(recomputed_statistics["recomputed"]) == 1);
	CHECK(int64_t(recomputed_statistics["evaluate_calls"]) == 2);
	CHECK(int64_t(recomputed_statistics["evaluate_first_key_calls"]) == 2);
	rig.submit();
	rig.processor->complete_and_publish();
	const Dictionary next_statistics = rig.processor->get_statistics();
	check_phase_diagnostics(next_statistics);
	CHECK(int64_t(next_statistics["recomputed"]) == 0);
	CHECK(int64_t(next_statistics["evaluate_calls"]) == 1);
	CHECK(int64_t(next_statistics["evaluate_first_key_calls"]) == 0);
	CHECK(int64_t(next_statistics["frame"]) == int64_t(recomputed_statistics["frame"]) + 1);
}

TEST_CASE("[SceneTree][CZAnimation] Phase diagnostics cover empty submissions and completed worker entries") {
	PhaseDiagnosticsEnvironment environment(true);
	Ref<AnimationBatchProcessor> processor;
	processor.instantiate();
	CHECK(processor->submit(1, 2) != 0);
	const Dictionary empty_statistics = processor->get_statistics();
	check_phase_diagnostics(empty_statistics);
	CHECK(int64_t(empty_statistics["evaluate_calls"]) == 0);
	CHECK(int64_t(empty_statistics["evaluate_total_us"]) == 0);
	struct Rigs {
		LocalVector<Rig *> items;
		~Rigs() {
			for (Rig *rig : items) {
				memdelete(rig);
			}
		}
	} rigs;
	for (int i = 0; i < 16; i++) {
		Rig *rig = memnew(Rig(processor));
		rigs.items.push_back(rig);
		REQUIRE(processor->queue_update(rig->handle, 0.1));
	}
	CHECK(processor->submit(1, 2) != 0);
	processor->complete();
	CHECK_FALSE(processor->get_statistics().has("evaluate_total_us"));
	processor->complete_and_publish();
	const Dictionary statistics = processor->get_statistics();
	check_phase_diagnostics(statistics);
	CHECK(int64_t(statistics["evaluate_calls"]) == 16);
	CHECK(int64_t(statistics["pawns_evaluated"]) == 16);
	CHECK(int64_t(statistics["workers"]) == MIN(2, MAX(1, WorkerThreadPool::get_singleton()->get_thread_count())));
	for (const Rig *rig : rigs.items) {
		CHECK(rig->observer->count == 1);
		CHECK(rig->skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(1, 0, 0)));
	}
}

TEST_CASE("[SceneTree][CZAnimation] Latched pose fastpaths preserve mixed tracks and first loop seek events") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(false);
	Ref<AnimationBatchProcessor> disabled;
	disabled.instantiate();
	environment.set_enabled(true);
	Ref<AnimationBatchProcessor> enabled;
	enabled.instantiate();
	environment.set_enabled(false);
	CHECK_FALSE(bool(disabled->get_statistics()["pose_fastpaths_enabled"]));
	CHECK(bool(enabled->get_statistics()["pose_fastpaths_enabled"]));
	Rig original;
	Rig legacy(disabled, { "rotate_y" });
	Rig fast(enabled, { "rotate_y" });
	AuxiliaryTracks original_aux(original);
	AuxiliaryTracks legacy_aux(legacy);
	AuxiliaryTracks fast_aux(fast);
	CHECK_FALSE(CZAnimationBridge::can_skip_tracks_early(original.tree));
	CHECK_FALSE(CZAnimationBridge::can_skip_tracks_early(fast.tree));
	for (Rig *rig : { &original, &legacy, &fast }) {
		rig->processor->unregister_tree(rig->handle);
		rig->player->set_callback_mode_process(AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
		rig->player->set_callback_mode_method(AnimationMixer::ANIMATION_CALLBACK_MODE_METHOD_IMMEDIATE);
		rig->player->set_deterministic(true);
		rig->player->play("Move");
	}
	for (Rig *rig : { &legacy, &fast }) {
		REQUIRE(rig->pose->capture(rig->skeleton));
		rig->handle = rig->processor->register_player(rig->player, { rig->pose }, { "rotate_y" });
		REQUIRE(rig->handle != 0);
		rig->player->connect(SNAME("mixer_applied"), callable_mp(rig->observer.ptr(), &AppliedCounter::on_applied));
	}
	const double deltas[] = { 0.1, 0.25, 0.8, 0 };
	const real_t expected_rotations[] = { 0.125, 0.25, 0.5, 0.625 };
	for (int step = 0; step < 4; step++) {
		if (step == 3) {
			for (Rig *rig : { &original, &legacy, &fast }) {
				rig->player->seek(0.8);
			}
		}
		original.player->advance(deltas[step]);
		// Preparing another processor captures native inputs and may legally
		// synchronize older processors. Test Complete before any such callout.
		for (int variant = 0; variant < 2; variant++) {
			Rig *rig = variant == 0 ? &legacy : &fast;
			AuxiliaryTracks *aux = variant == 0 ? &legacy_aux : &fast_aux;
			const Vector3 rotation_before = aux->target->get_rotation();
			const Vector3 position_before = aux->target->get_position();
			const float blend_shape_before = aux->mesh_instance->get_blend_shape_value(0);
			rig->submit(deltas[step]);
			rig->processor->complete();
			CHECK(rig->observer->count == step);
			CHECK(aux->target->get_rotation().is_equal_approx(rotation_before));
			CHECK(aux->target->get_position().is_equal_approx(position_before));
			CHECK(Math::is_equal_approx(aux->mesh_instance->get_blend_shape_value(0), blend_shape_before));
			rig->processor->publish(rig->handle);
			rig->processor->publish(rig->handle);
			rig->processor->complete_and_publish();
			CHECK(rig->observer->count == step + 1);
			CHECK(rig->processor->was_evaluated(rig->handle));
			CHECK(rig->skeleton->get_bone_pose(0).is_equal_approx(original.skeleton->get_bone_pose(0)));
		}
		for (AuxiliaryTracks *aux : { &legacy_aux, &fast_aux }) {
			CHECK(aux->target->get_position().is_equal_approx(original_aux.target->get_position()));
			CHECK(aux->target->get_rotation().is_equal_approx(original_aux.target->get_rotation()));
			CHECK(Math::is_equal_approx(aux->target->get_rotation().y, expected_rotations[step]));
			CHECK(Math::is_equal_approx(aux->mesh_instance->get_blend_shape_value(0), original_aux.mesh_instance->get_blend_shape_value(0)));
		}
	}
	CHECK_FALSE(bool(disabled->get_statistics()["pose_fastpaths_enabled"]));
	CHECK(bool(enabled->get_statistics()["pose_fastpaths_enabled"]));
	CHECK(method_statistic(disabled, "evaluate_normalized_copy_elements") > 0);
	CHECK(method_statistic(enabled, "evaluate_normalized_copy_elements") == 0);
	CHECK(method_statistic(enabled, "evaluate_raw_weight_elements") > 0);
	CHECK(method_statistic(enabled, "evaluate_instance_track_skips") == 0);
	CHECK(method_statistic(enabled, "publish_instance_track_skips") == 0);
}

TEST_CASE("[SceneTree][CZAnimation] Early track skipping refreshes switched root motion paths") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(false);
	Rig legacy;
	environment.set_enabled(true);
	Rig fast;
	Rig original;
	original.processor->unregister_tree(original.handle);
	for (Rig *rig : { &original, &legacy, &fast }) {
		const int track = rig->animation->add_track(Animation::TYPE_POSITION_3D);
		rig->animation->track_set_path(track, NodePath("Skeleton3D:Finger"));
		rig->animation->position_track_insert_key(track, 0, Vector3());
		rig->animation->position_track_insert_key(track, 1, Vector3(0, 4, 0));
	}
	for (const NodePath &path : { NodePath("Skeleton3D:Root"), NodePath("Skeleton3D:Root"), NodePath("Skeleton3D:Finger"), NodePath("Skeleton3D:Finger"), NodePath(), NodePath("Skeleton3D:Root"), NodePath() }) {
		for (Rig *rig : { &original, &legacy, &fast }) {
			rig->tree->set_root_motion_track(path);
		}
		original.tree->advance(0.1);
		for (Rig *rig : { &legacy, &fast }) {
			rig->submit();
			rig->processor->complete_and_publish();
			CHECK(rig->processor->was_evaluated(rig->handle));
			for (int bone = 0; bone < 2; bone++) {
				CHECK(rig->skeleton->get_bone_pose(bone).is_equal_approx(original.skeleton->get_bone_pose(bone)));
			}
			CHECK(rig->tree->get_root_motion_position().is_equal_approx(original.tree->get_root_motion_position()));
			CHECK(rig->tree->get_root_motion_position_accumulator().is_equal_approx(original.tree->get_root_motion_position_accumulator()));
			if (path.is_empty()) {
				CHECK(rig->tree->get_root_motion_position().is_zero_approx());
				CHECK(rig->tree->get_root_motion_position_accumulator().is_zero_approx());
			}
			CHECK((method_statistic(rig->processor, "evaluate_instance_track_skips") > 0) == (rig == &fast && path.is_empty()));
			CHECK(method_statistic(rig->processor, "publish_instance_track_skips") == 0);
			CHECK(method_statistic(rig->processor, "publish_eventless_calls") == int64_t(rig == &fast));
		}
	}
}

TEST_CASE("[SceneTree][CZAnimation] Bone-only instance skips preserve Player first keys and reject non-deterministic mixing") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(false);
	Rig legacy;
	environment.set_enabled(true);
	Rig fast;
	Rig original;
	for (Rig *rig : { &original, &legacy, &fast }) {
		rig->processor->unregister_tree(rig->handle);
		rig->player->set_callback_mode_process(AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
		rig->player->set_deterministic(true);
		rig->player->play("Move");
	}
	for (Rig *rig : { &legacy, &fast }) {
		REQUIRE(rig->pose->capture(rig->skeleton));
		rig->handle = rig->processor->register_player(rig->player, { rig->pose }, {});
		REQUIRE(rig->handle != 0);
		rig->player->connect(SNAME("mixer_applied"), callable_mp(rig->observer.ptr(), &AppliedCounter::on_applied));
	}
	CHECK_FALSE(CZAnimationBridge::skip_animation_instance(fast.player, fast.animation));
	for (int step = 0; step < 4; step++) {
		if (step >= 2) {
			for (Rig *rig : { &original, &legacy, &fast }) {
				rig->player->set_deterministic(step != 2);
			}
		}
		original.player->advance(0.1);
		for (Rig *rig : { &legacy, &fast }) {
			rig->submit();
			rig->processor->complete();
			CHECK(rig->observer->count == step);
			rig->processor->publish(rig->handle);
			rig->processor->publish(rig->handle);
			rig->processor->complete_and_publish();
			CHECK(rig->observer->count == step + 1);
			CHECK(rig->processor->was_evaluated(rig->handle));
			CHECK(rig->skeleton->get_bone_pose(0).is_equal_approx(original.skeleton->get_bone_pose(0)));
			const bool skipped = rig == &fast && step != 2;
			CHECK(method_statistic(rig->processor, "evaluate_owned_weight_copy_elements") == method_statistic(rig->processor, "evaluate_raw_weight_elements"));
			CHECK(method_statistic(rig->processor, "evaluate_borrowed_weight_calls") == 0);
			CHECK(method_statistic(rig->processor, "publish_eventless_calls") == 0);
			CHECK((method_statistic(rig->processor, "evaluate_instance_track_skips") > 0) == skipped);
			CHECK((method_statistic(rig->processor, "publish_instance_track_skips") > 0) == skipped);
			if (skipped) {
				CHECK(method_statistic(rig->processor, "evaluate_instance_track_skips") == method_statistic(rig->processor, "evaluate_animation_instances"));
				CHECK(method_statistic(rig->processor, "publish_instance_track_skips") == method_statistic(rig->processor, "evaluate_animation_instances") + method_statistic(rig->processor, "evaluate_first_key_calls"));
			}
		}
		CHECK(method_statistic(legacy.processor, "evaluate_raw_weight_elements") == method_statistic(fast.processor, "evaluate_raw_weight_elements"));
		if (step == 0) {
			CHECK(method_statistic(fast.processor, "evaluate_first_key_calls") == 1);
		}
	}
}

TEST_CASE("[SceneTree][CZAnimation] Instance track certificates rebuild for enabled event and replacement animations") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(false);
	Rig legacy(Ref<AnimationBatchProcessor>(), { "rotate_y" });
	environment.set_enabled(true);
	Rig fast(Ref<AnimationBatchProcessor>(), { "rotate_y" });
	Rig original;
	original.processor->unregister_tree(original.handle);
	Rig *rigs[] = { &original, &legacy, &fast };
	Node3D *targets[3] = {};
	int event_tracks[3] = {};
	for (int index = 0; index < 3; index++) {
		targets[index] = memnew(Node3D);
		targets[index]->set_name("Target");
		rigs[index]->root->add_child(targets[index]);
		rigs[index]->tree->set_callback_mode_method(AnimationMixer::ANIMATION_CALLBACK_MODE_METHOD_IMMEDIATE);
	}
	for (int step = 0; step < 8; step++) {
		for (int index = 0; index < 3; index++) {
			Rig &rig = *rigs[index];
			if (step == 2) {
				rig.animation->track_set_enabled(0, false);
			} else if (step == 3) {
				rig.animation->track_set_enabled(0, true);
				rig.animation->position_track_insert_key(0, 0.6, Vector3(8, 0, 0));
			} else if (step == 4) {
				const int track = rig.animation->add_track(Animation::TYPE_METHOD);
				event_tracks[index] = track;
				rig.animation->track_set_path(track, NodePath("Target"));
				Dictionary key;
				key["method"] = StringName("rotate_y");
				key["args"] = Array{ 0.125 };
				rig.animation->track_insert_key(track, 0.45, key);
			} else if (step == 5) {
				rig.animation->track_set_enabled(event_tracks[index], false);
			} else if (step == 6) {
				Ref<Animation> replacement;
				replacement.instantiate();
				replacement->set_length(1);
				replacement->set_loop_mode(Animation::LOOP_LINEAR);
				const int track = replacement->add_track(Animation::TYPE_POSITION_3D);
				replacement->track_set_path(track, NodePath("Skeleton3D:Root"));
				replacement->position_track_insert_key(track, 0, Vector3());
				replacement->position_track_insert_key(track, 1, Vector3(20, 0, 0));
				REQUIRE(rig.player->get_animation_library(StringName())->add_animation("Move", replacement) == OK);
				rig.animation = replacement;
			}
		}
		original.tree->advance(0.1);
		for (int index = 1; index < 3; index++) {
			Rig &rig = *rigs[index];
			const Vector3 rotation_before = targets[index]->get_rotation();
			rig.submit();
			rig.processor->complete();
			CHECK(rig.observer->count == step);
			CHECK(targets[index]->get_rotation().is_equal_approx(rotation_before));
			rig.processor->publish(rig.handle);
			rig.processor->publish(rig.handle);
			rig.processor->complete_and_publish();
			CHECK(rig.observer->count == step + 1);
			CHECK(rig.processor->was_evaluated(rig.handle));
			CHECK(rig.skeleton->get_bone_pose(0).is_equal_approx(original.skeleton->get_bone_pose(0)));
			CHECK(targets[index]->get_rotation().is_equal_approx(targets[0]->get_rotation()));
			CHECK(Math::is_equal_approx(targets[index]->get_rotation().y, step >= 4 ? real_t(0.125) : real_t(0)));
			CHECK((method_statistic(rig.processor, "evaluate_instance_track_skips") > 0) == (index == 2 && step != 2 && step != 4));
			CHECK(method_statistic(rig.processor, "publish_instance_track_skips") == 0);
			CHECK(method_statistic(rig.processor, "publish_eventless_calls") == int64_t(index == 2 && step != 4));
			if (index == 2 && (step == 1 || step == 7)) {
				CHECK(method_statistic(rig.processor, "zero_weight_mask_rebuilds") == 0);
			}
		}
	}
}

class MixerDeletion : public RefCounted {
	GDCLASS(MixerDeletion, RefCounted);

protected:
	static void _bind_methods() {}

public:
	AnimationTree **target = nullptr;
	void remove() {
		AnimationTree *mixer = *target;
		*target = nullptr;
		if (mixer) {
			memdelete(mixer);
		}
	}
};

struct ExpectedSignalFreeError {
	ErrorHandlerList handler;
	bool previous_print_error = CoreGlobals::print_error_enabled;
	int expected = 0;
	int unexpected = 0;

	static void detect(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		ExpectedSignalFreeError *self = static_cast<ExpectedSignalFreeError *>(p_self);
		if (p_type == ERR_HANDLER_ERROR && String(p_error).contains("was freed or unreferenced while a signal is being emitted from it.")) {
			self->expected++;
		} else {
			self->unexpected++;
		}
	}
	ExpectedSignalFreeError() {
		handler.errfunc = detect;
		handler.userdata = this;
		add_error_handler(&handler);
		CoreGlobals::print_error_enabled = false;
	}
	~ExpectedSignalFreeError() {
		CoreGlobals::print_error_enabled = previous_print_error;
		remove_error_handler(&handler);
	}
};

TEST_CASE("[SceneTree][CZAnimation] Root motion setters finish readers and survive publication or cache clear deletion") {
	Rig rig;
	rig.submit();
	rig.tree->set_root_motion_track(NodePath("Skeleton3D:Root"));
	CHECK(rig.observer->count == 1);
	CHECK(rig.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(1, 0, 0)));
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(rig.observer->count == 1);
	rig.submit();
	rig.tree->set_root_motion_local(true);
	CHECK(rig.observer->count == 2);
	CHECK(rig.tree->is_root_motion_local());
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(rig.observer->count == 2);
	Ref<AppliedCounter> cache_clears;
	cache_clears.instantiate();
	rig.tree->connect(SNAME("caches_cleared"), callable_mp(cache_clears.ptr(), &AppliedCounter::on_applied));
	rig.tree->set_root_motion_track(NodePath("Skeleton3D:Root"));
	CHECK(cache_clears->count == 0);
	rig.tree->set_root_motion_track(NodePath());
	CHECK(cache_clears->count == 1);
	for (int callout = 0; callout < 3; callout++) {
		Rig doomed;
		const ObjectID mixer_id = doomed.tree->get_instance_id();
		Ref<MixerDeletion> deletion;
		deletion.instantiate();
		deletion->target = &doomed.tree;
		if (callout < 2) {
			doomed.submit();
			doomed.tree->connect(SNAME("mixer_applied"), callable_mp(deletion.ptr(), &MixerDeletion::remove), Object::CONNECT_ONE_SHOT);
		} else {
			doomed.tree->connect(SNAME("caches_cleared"), callable_mp(deletion.ptr(), &MixerDeletion::remove), Object::CONNECT_ONE_SHOT);
		}
		{
			// Deliberately exercise immediate deletion during a signal rather
			// than the recommended queue_free(). Capture its one expected error.
			ExpectedSignalFreeError errors;
			if (callout == 1) {
				doomed.tree->set_root_motion_local(true);
			} else {
				doomed.tree->set_root_motion_track(NodePath("Skeleton3D:Root"));
			}
			CHECK(errors.expected == 1);
			CHECK(errors.unexpected == 0);
		}
		CHECK(doomed.tree == nullptr);
		CHECK(ObjectDB::get_instance(mixer_id) == nullptr);
	}
}

class LibraryBeforeApply : public RefCounted {
	GDCLASS(LibraryBeforeApply, RefCounted);

protected:
	static void _bind_methods() {}

public:
	Ref<AnimationLibrary> library;
	Ref<Animation> expected_animation;
	AnimationTree *tree = nullptr;
	StringName name;
	bool expected_library = false;
	bool matches = true;
	int calls = 0;

	void watch_animation(const Ref<AnimationLibrary> &p_library, const StringName &p_name, const Ref<Animation> &p_expected) {
		library = p_library;
		expected_animation = p_expected;
		tree = nullptr;
		name = p_name;
		matches = true;
		calls = 0;
	}
	void watch_library(AnimationTree *p_tree, const StringName &p_name, bool p_present) {
		tree = p_tree;
		name = p_name;
		expected_library = p_present;
		matches = true;
		calls = 0;
	}
	void applied() {
		calls++;
		if (tree) {
			matches &= tree->has_animation_library(name) == expected_library;
		} else {
			const bool present = library->has_animation(name);
			matches &= present == expected_animation.is_valid();
			if (present && expected_animation.is_valid()) {
				matches &= library->get_animation(name) == expected_animation;
			}
		}
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
	Ref<Animation> mutate_animation;
	bool nested_publish = false;
	bool captured = false;
	int calls = 0;
	int mutations = 0;

	void submit() {
		calls++;
		for (int64_t handle : handles) {
			REQUIRE(processor->queue_update(handle, 0.1));
		}
		processor->submit(8, 2);
		if (nested_publish) {
			processor->complete_and_publish();
		}
		if (mutate_animation.is_valid()) {
			const Ref<Animation> animation = mutate_animation;
			mutate_animation.unref();
			animation->position_track_insert_key(0, 0.5, Vector3(8, 0, 0));
			mutations++;
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
	for (bool fastpaths : { false, true }) {
		PoseFastpathsEnvironment environment(fastpaths);
		Rig first;
		Rig second;
		Ref<ReentryAction> action;
		action.instantiate();
		action->processor = second.processor;
		action->handles.push_back(second.handle);
		action->nested_publish = true;
		action->mutate_animation = first.animation;
		first.skeleton->connect(SNAME("skeleton_updated"), callable_mp(action.ptr(), &ReentryAction::submit));
		first.submit();
		first.processor->complete_and_publish();
		CHECK(action->calls == 1);
		CHECK(action->mutations == 1);
		CHECK(first.animation->track_get_key_count(0) == 3);
		CHECK(first.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(1, 0, 0)));
		CHECK(second.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(1, 0, 0)));
	}
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
	for (bool fastpaths : { false, true }) {
		PoseFastpathsEnvironment environment(fastpaths);
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

TEST_CASE("[SceneTree][CZAnimation] Method policy certificates invalidate on method keys enabled tracks and new clips") {
	Rig rig(Ref<AnimationBatchProcessor>(), { "rotate_y" });
	const Ref<AnimationLibrary> library = rig.tree->get_animation_library(StringName());
	const Ref<Animation> still = library->get_animation("Still");
	const Ref<Animation> new_unsafe = make_method_animation("rotate_x");
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->was_evaluated(rig.handle));
	CHECK(method_statistic(rig.processor, "method_validation_scans") == 1);
	CHECK(method_statistic(rig.processor, "method_validation_tracks") == 2);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(method_statistic(rig.processor, "method_validation_cache_hits") == 1);
	CHECK(method_statistic(rig.processor, "method_validation_scans") == 0);
	CHECK(method_statistic(rig.processor, "method_validation_tracks") == 0);

	const int method = still->add_track(Animation::TYPE_METHOD);
	still->track_set_path(method, NodePath("."));
	Dictionary key;
	key["method"] = StringName("rotate_y");
	key["args"] = Array{ 0.125 };
	still->track_insert_key(method, 0.5, key);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->was_evaluated(rig.handle));
	CHECK(method_statistic(rig.processor, "method_validation_scans") == 1);
	CHECK(method_statistic(rig.processor, "method_validation_keys") == 1);

	// Same track/key count and target path: content changes must still expire
	// the certificate, including clips not selected by the current graph.
	key["method"] = StringName("rotate_x");
	still->track_set_key_value(method, 0, key);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->was_evaluated(rig.handle));
	CHECK(rig.processor->get_fallback_reason(rig.handle) == "Immediate method track requires synchronous evaluation.");
	CHECK(method_statistic(rig.processor, "method_validation_scans") == 1);
	REQUIRE(rig.pose->capture(rig.skeleton));
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->was_evaluated(rig.handle));
	CHECK(method_statistic(rig.processor, "method_validation_cache_hits") == 1);
	CHECK(method_statistic(rig.processor, "method_validation_scans") == 0);

	still->track_set_enabled(method, false);
	REQUIRE(rig.pose->capture(rig.skeleton));
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->was_evaluated(rig.handle));
	CHECK(method_statistic(rig.processor, "method_validation_scans") == 1);
	still->track_set_enabled(method, true);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->was_evaluated(rig.handle));
	key["method"] = StringName("rotate_y");
	still->track_set_key_value(method, 0, key);
	REQUIRE(rig.pose->capture(rig.skeleton));
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->was_evaluated(rig.handle));

	// A new library entry only sets cache_valid=false in the official mixer;
	// it does not necessarily call invalidate_binding_cache().
	REQUIRE(library->add_animation("Unused", new_unsafe) == OK);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->was_evaluated(rig.handle));
	CHECK(method_statistic(rig.processor, "method_validation_scans") == 1);
	library->remove_animation("Unused");
	REQUIRE(rig.pose->capture(rig.skeleton));
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->was_evaluated(rig.handle));
}

TEST_CASE("[SceneTree][CZAnimation] Animation library mutations finish pending publication before changing contents") {
	Rig rig;
	Rig other;
	const Ref<AnimationLibrary> library = rig.tree->get_animation_library(StringName());
	const Ref<Animation> extra = rig.animation->duplicate(true);
	const Ref<Animation> replacement = rig.animation->duplicate(true);
	Ref<LibraryBeforeApply> check;
	check.instantiate();
	rig.tree->connect(SNAME("mixer_applied"), callable_mp(check.ptr(), &LibraryBeforeApply::applied));
	check->watch_animation(library, "Extra", Ref<Animation>());
	rig.submit();
	{
		// An unrelated Player mutation scope must not suppress the shared
		// resource barrier, even though its own mixer has no pending work.
		CZAnimationMutationScope scope(other.tree);
		REQUIRE(library->add_animation("Extra", extra) == OK);
	}
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(check->matches);

	check->watch_animation(library, "Extra", extra);
	rig.submit();
	REQUIRE(library->add_animation("Extra", replacement) == OK);
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(check->matches);
	CHECK(library->get_animation("Extra") == replacement);

	check->watch_animation(library, "Extra", replacement);
	rig.submit();
	library->rename_animation("Extra", "Renamed");
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(check->matches);
	CHECK_FALSE(library->has_animation("Extra"));
	CHECK(library->get_animation("Renamed") == replacement);

	check->watch_animation(library, "Renamed", replacement);
	rig.submit();
	library->remove_animation("Renamed");
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(check->matches);
	CHECK_FALSE(library->has_animation("Renamed"));
}

TEST_CASE("[SceneTree][CZAnimation] Tree library changes publish the old frame before writing membership") {
	Rig rig;
	Ref<AnimationLibrary> extra;
	extra.instantiate();
	REQUIRE(extra->add_animation("ExtraMove", rig.animation->duplicate(true)) == OK);
	Ref<LibraryBeforeApply> check;
	check.instantiate();
	rig.tree->connect(SNAME("mixer_applied"), callable_mp(check.ptr(), &LibraryBeforeApply::applied));
	check->watch_library(rig.tree, "Extra", false);
	rig.submit();
	REQUIRE(rig.tree->add_animation_library("Extra", extra) == OK);
	CHECK(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(check->matches);
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(check->calls == 1);

	check->watch_library(rig.tree, "Extra", true);
	rig.submit();
	rig.tree->rename_animation_library("Extra", "Renamed");
	CHECK(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(check->matches);
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(rig.tree->has_animation_library("Renamed"));
	CHECK(rig.tree->has_animation("Renamed/ExtraMove"));
	CHECK_FALSE(rig.tree->has_animation("Extra/ExtraMove"));

	check->watch_library(rig.tree, "Renamed", true);
	rig.submit();
	rig.tree->remove_animation_library("Renamed");
	CHECK(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(check->matches);
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK_FALSE(rig.tree->has_animation_library("Renamed"));
}

TEST_CASE("[SceneTree][CZAnimation] Shared animation method certificates remain specific to each registration policy") {
	Rig allowed(Ref<AnimationBatchProcessor>(), { "rotate_y" });
	Rig rejected;
	const Ref<Animation> shared = make_method_animation("rotate_y");
	REQUIRE(allowed.tree->get_animation_library(StringName())->add_animation("Unused", shared) == OK);
	REQUIRE(rejected.tree->get_animation_library(StringName())->add_animation("Unused", shared) == OK);
	allowed.submit();
	rejected.submit();
	allowed.processor->complete_and_publish();
	rejected.processor->complete_and_publish();
	CHECK(allowed.processor->was_evaluated(allowed.handle));
	CHECK_FALSE(rejected.processor->was_evaluated(rejected.handle));
	REQUIRE(rejected.pose->capture(rejected.skeleton));
	allowed.submit();
	rejected.submit();
	allowed.processor->complete_and_publish();
	rejected.processor->complete_and_publish();
	CHECK(allowed.processor->was_evaluated(allowed.handle));
	CHECK_FALSE(rejected.processor->was_evaluated(rejected.handle));
	CHECK(method_statistic(allowed.processor, "method_validation_cache_hits") == 1);
	CHECK(method_statistic(rejected.processor, "method_validation_cache_hits") == 1);
	CHECK(method_statistic(allowed.processor, "method_validation_scans") == 0);
	CHECK(method_statistic(rejected.processor, "method_validation_scans") == 0);
}

#ifdef THREADS_ENABLED
TEST_CASE("[SceneTree][CZAnimation] Independent worker library construction does not join pending scene work") {
	Rig rig;
	rig.submit();
	Ref<AnimationLibrary> built;
	Thread worker;
	worker.start([](void *p_userdata) {
		Ref<AnimationLibrary> &library = *static_cast<Ref<AnimationLibrary> *>(p_userdata);
		library.instantiate();
		Ref<Animation> clip;
		clip.instantiate();
		library->add_animation("First", clip);
		library->rename_animation("First", "Ready");
		library->remove_animation("Ready");
		library->add_animation("Ready", clip);
	},
			&built);
	REQUIRE(worker.is_started());
	worker.wait_to_finish();
	REQUIRE(built.is_valid());
	CHECK(built->has_animation("Ready"));
	CHECK(rig.processor->is_pending());
	CHECK(rig.observer->count == 0);
	rig.processor->complete_and_publish();
	CHECK(rig.observer->count == 1);
}
#endif

TEST_CASE("[SceneTree][CZAnimation] Empty library data replacement publishes readers and expires old content") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	Rig rig(Ref<AnimationBatchProcessor>(), { "rotate_y" });
	rig.tree->set_callback_mode_method(AnimationMixer::ANIMATION_CALLBACK_MODE_METHOD_IMMEDIATE);
	const Ref<AnimationLibrary> library = rig.tree->get_animation_library(StringName());
	const Ref<Animation> still = library->get_animation("Still");
	const int method = rig.animation->add_track(Animation::TYPE_METHOD);
	rig.animation->track_set_path(method, NodePath("."));
	Dictionary key;
	key["method"] = StringName("rotate_y");
	key["args"] = Array{ 0.125 };
	rig.animation->track_insert_key(method, 0.05, key);
	Ref<LibraryBeforeApply> check;
	check.instantiate();
	check->watch_animation(library, "Move", rig.animation);
	rig.tree->connect(SNAME("mixer_applied"), callable_mp(check.ptr(), &LibraryBeforeApply::applied));
	rig.submit();
	library->set("_data", Dictionary());
	CHECK_FALSE(rig.processor->is_pending());
	CHECK(check->calls == 1);
	CHECK(check->matches);
	CHECK(Math::is_equal_approx(rig.root->get_rotation().y, real_t(0.125)));
	CHECK(library->get_animation_list_size() == 0);
	CHECK_FALSE(rig.tree->has_animation("Move"));
	CHECK_FALSE(rig.tree->has_animation("Still"));
	rig.processor->complete_and_publish();
	CHECK(check->calls == 1);
	CHECK(Math::is_equal_approx(rig.root->get_rotation().y, real_t(0.125)));

	const Ref<Animation> replacement = rig.animation->duplicate(true);
	key["method"] = StringName("rotate_x");
	replacement->track_set_key_value(method, 0, key);
	replacement->position_track_insert_key(0, 1, Vector3(20, 0, 0));
	Dictionary data;
	data["Move"] = replacement;
	data["Still"] = still;
	library->set("_data", data);
	CHECK(rig.tree->has_animation("Move"));
	CHECK(rig.tree->get_animation("Move") == replacement);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK_FALSE(rig.processor->was_evaluated(rig.handle));
	CHECK(rig.processor->get_fallback_reason(rig.handle) == "Immediate method track requires synchronous evaluation.");
	CHECK(method_statistic(rig.processor, "method_validation_scans") == 1);
	CHECK(rig.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(4, 0, 0)));
}

class LibraryDataReentry : public RefCounted {
	GDCLASS(LibraryDataReentry, RefCounted);

protected:
	static void _bind_methods() {}

public:
	Dictionary *input = nullptr;
	Ref<AnimationLibrary> *external_library = nullptr;
	ObjectID library_id;
	Ref<Animation> inserted;
	int published = 0;
	int removed = 0;
	int incoming_added = 0;
	bool entered_removal = false;
	bool retained_inserted = true;

	void on_applied() {
		published++;
		input->clear();
		external_library->unref();
	}
	void on_removed(const StringName &p_name) {
		removed++;
		if (entered_removal) {
			return;
		}
		entered_removal = true;
		AnimationLibrary *library = Object::cast_to<AnimationLibrary>(ObjectDB::get_instance(library_id));
		REQUIRE(library);
		// Delete a future old entry and insert a name absent from the fixed list.
		for (const StringName &name : { StringName("A"), StringName("B") }) {
			if (library->has_animation(name)) {
				library->remove_animation(name);
			}
		}
		REQUIRE(library->add_animation("DuringRemoval", inserted) == OK);
		input->clear();
		external_library->unref();
	}
	void on_added(const StringName &p_name) {
		if (p_name == SNAME("IncomingA") || p_name == SNAME("IncomingB")) {
			incoming_added++;
			AnimationLibrary *library = Object::cast_to<AnimationLibrary>(ObjectDB::get_instance(library_id));
			REQUIRE(library);
			retained_inserted &= library->has_animation("DuringRemoval") && library->get_animation(p_name).is_valid();
		}
	}
};

TEST_CASE("[SceneTree][CZAnimation] Library data snapshots survive pending input mutation and bounded removal reentry") {
	Rig rig;
	Ref<AnimationLibrary> library;
	library.instantiate();
	REQUIRE(library->add_animation("A", rig.animation) == OK);
	REQUIRE(library->add_animation("B", rig.animation) == OK);
	Dictionary data;
	data["IncomingA"] = rig.animation;
	data["IncomingB"] = rig.animation->duplicate(true);
	Ref<LibraryDataReentry> callback;
	callback.instantiate();
	callback->input = &data;
	callback->external_library = &library;
	callback->library_id = library->get_instance_id();
	callback->inserted = rig.animation;
	library->connect(SNAME("animation_removed"), callable_mp(callback.ptr(), &LibraryDataReentry::on_removed));
	library->connect(SNAME("animation_added"), callable_mp(callback.ptr(), &LibraryDataReentry::on_added));
	rig.tree->connect(SNAME("mixer_applied"), callable_mp(callback.ptr(), &LibraryDataReentry::on_applied), Object::CONNECT_ONE_SHOT);
	rig.submit();
	library->set("_data", data);
	CHECK(data.is_empty());
	CHECK(library.is_null());
	CHECK(callback->published == 1);
	CHECK(callback->removed == 2);
	CHECK(callback->incoming_added == 2);
	CHECK(callback->retained_inserted);
	CHECK(ObjectDB::get_instance(callback->library_id) == nullptr);
	CHECK_FALSE(rig.processor->is_pending());
	rig.processor->complete_and_publish();
	CHECK(callback->published == 1);
}

static void install_zero_certificate_graph(Rig &p_rig) {
	Ref<AnimationNodeBlendTree> graph;
	graph.instantiate();
	Ref<AnimationNodeAnimation> still;
	still.instantiate();
	still->set_animation("Still");
	Ref<AnimationNodeBlend2> blend;
	blend.instantiate();
	blend->set_filter_enabled(true);
	blend->set_filter_path(NodePath("Skeleton3D:Root"), true);
	graph->add_node("Move", p_rig.animation_node);
	graph->add_node("Still", still);
	graph->add_node("Masked", blend);
	graph->connect_node("Masked", 0, "Move");
	graph->connect_node("Masked", 1, "Still");
	graph->connect_node("output", 0, "Masked");
	p_rig.tree->set_root_animation_node(graph);
	p_rig.tree->set("parameters/Masked/blend_amount", 1.0);
}

TEST_CASE("[SceneTree][CZAnimation] Zero bone certificates preserve filtered auxiliary events mutation and root motion") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	for (bool deterministic : { true, false }) {
		PoseFastpathsEnvironment environment(false);
		Rig legacy(Ref<AnimationBatchProcessor>(), { "rotate_y" });
		environment.set_enabled(true);
		Rig fast(Ref<AnimationBatchProcessor>(), { "rotate_y" });
		Rig original;
		original.processor->unregister_tree(original.handle);
		AuxiliaryTracks original_aux(original);
		AuxiliaryTracks legacy_aux(legacy);
		AuxiliaryTracks fast_aux(fast);
		for (Rig *rig : { &original, &legacy, &fast }) {
			rig->tree->set_deterministic(deterministic);
			install_zero_certificate_graph(*rig);
		}
		for (int step = 0; step < 7; step++) {
			for (Rig *rig : { &original, &legacy, &fast }) {
				rig->tree->set("parameters/Masked/blend_amount", step == 1 ? 0.35 : 1.0);
				if (step == 2) {
					rig->animation->track_set_enabled(0, false);
				} else if (step == 3) {
					rig->animation->track_set_enabled(0, true);
					rig->animation->position_track_insert_key(0, 0.6, Vector3(7, 0, 0));
				} else if (step == 4) {
					rig->tree->set_root_motion_track(NodePath("Skeleton3D:Root"));
				} else if (step == 5) {
					rig->tree->set_root_motion_track(NodePath());
				}
			}
			original.tree->advance(0.1);
			for (int variant = 0; variant < 2; variant++) {
				Rig *rig = variant == 0 ? &legacy : &fast;
				AuxiliaryTracks *aux = variant == 0 ? &legacy_aux : &fast_aux;
				const Vector3 prior_position = aux->target->get_position();
				const Vector3 prior_rotation = aux->target->get_rotation();
				const float prior_shape = aux->mesh_instance->get_blend_shape_value(0);
				const int prior_events = rig->observer->count;
				rig->submit();
				rig->processor->complete();
				CHECK(aux->target->get_position().is_equal_approx(prior_position));
				CHECK(aux->target->get_rotation().is_equal_approx(prior_rotation));
				CHECK(Math::is_equal_approx(aux->mesh_instance->get_blend_shape_value(0), prior_shape));
				CHECK(rig->observer->count == prior_events);
				rig->processor->publish(rig->handle);
				rig->processor->publish(rig->handle);
				rig->processor->complete_and_publish();
				CHECK(rig->processor->was_evaluated(rig->handle));
				CHECK(rig->observer->count == prior_events + 1);
				CHECK(rig->skeleton->get_bone_pose(0).is_equal_approx(original.skeleton->get_bone_pose(0)));
				CHECK(rig->tree->get_root_motion_position().is_equal_approx(original.tree->get_root_motion_position()));
				CHECK(rig->tree->get_root_motion_position_accumulator().is_equal_approx(original.tree->get_root_motion_position_accumulator()));
				CHECK(aux->target->get_position().is_equal_approx(original_aux.target->get_position()));
				CHECK(aux->target->get_rotation().is_equal_approx(original_aux.target->get_rotation()));
				CHECK(Math::is_equal_approx(aux->mesh_instance->get_blend_shape_value(0), original_aux.mesh_instance->get_blend_shape_value(0)));
				const int64_t skips = method_statistic(rig->processor, "sample_certified_zero_weight_calls");
				if (variant == 0 || !deterministic || step == 1) {
					CHECK(skips == 0);
				} else {
					CHECK(skips > 0);
				}
				CHECK(skips <= method_statistic(rig->processor, "sample_zero_bone_weight_calls"));
			}
			for (const char *key : { "sample_pose_instance_calls", "sample_zero_bone_weight_calls", "sample_no_written_bone_calls", "sample_empty_binding_calls", "evaluate_animation_instances", "evaluate_raw_weight_elements" }) {
				CHECK(method_statistic(legacy.processor, key) == method_statistic(fast.processor, key));
			}
			if (deterministic) {
				const int64_t rebuilds = method_statistic(fast.processor, "zero_weight_mask_rebuilds");
				CHECK(rebuilds == int64_t(step != 1 && step != 6));
			} else {
				CHECK(method_statistic(fast.processor, "zero_weight_mask_rebuilds") == 0);
			}
		}
	}
}

TEST_CASE("[SceneTree][CZAnimation] Zero weight masks include every registered Pose and rebuild for bone LOD") {
	struct BoneLodRestore {
		bool enabled = SkeletonAnimationPose::is_bone_lod_enabled();
		BoneLodRestore() { SkeletonAnimationPose::set_bone_lod_enabled(true); }
		~BoneLodRestore() { SkeletonAnimationPose::set_bone_lod_enabled(enabled); }
	} bone_lod_restore;
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(false);
	Rig legacy;
	environment.set_enabled(true);
	Rig fast;
	Rig original;
	Rig *rigs[] = { &original, &legacy, &fast };
	Skeleton3D *secondary[3] = {};
	Ref<SkeletonAnimationPose> secondary_poses[2];
	for (int index = 0; index < 3; index++) {
		Rig &rig = *rigs[index];
		rig.processor->unregister_tree(rig.handle);
		secondary[index] = memnew(Skeleton3D);
		secondary[index]->set_name("Secondary");
		secondary[index]->add_bone("Root");
		secondary[index]->add_bone("Finger");
		secondary[index]->set_bone_parent(1, 0);
		rig.root->add_child(secondary[index]);
		const int track = rig.animation->add_track(Animation::TYPE_POSITION_3D);
		rig.animation->track_set_path(track, NodePath("Secondary:Finger"));
		rig.animation->position_track_insert_key(track, 0, Vector3());
		rig.animation->position_track_insert_key(track, 1, Vector3(0, 5, 0));
		install_zero_certificate_graph(rig);
		if (index > 0) {
			Ref<SkeletonAnimationPose> &second_pose = secondary_poses[index - 1];
			second_pose.instantiate();
			second_pose->configure_bone_lod({ "Finger" }, { 1 });
			REQUIRE(second_pose->capture(secondary[index]));
			REQUIRE(rig.pose->capture(rig.skeleton));
			rig.handle = rig.processor->register_tree(rig.tree, { rig.pose, second_pose }, {});
			REQUIRE(rig.handle != 0);
		}
	}
	for (int step = 0; step < 4; step++) {
		if (step >= 2) {
			for (const Ref<SkeletonAnimationPose> &pose : secondary_poses) {
				pose->set_bone_lod(step == 2 ? 1 : 0);
			}
		}
		original.tree->advance(0.1);
		for (int variant = 0; variant < 2; variant++) {
			Rig &rig = *rigs[variant + 1];
			rig.submit();
			rig.processor->complete_and_publish();
			CHECK(rig.processor->was_evaluated(rig.handle));
			CHECK(rig.skeleton->get_bone_pose(0).is_equal_approx(original.skeleton->get_bone_pose(0)));
			if (step == 2) {
				CHECK(secondary_poses[variant]->get_active_bone_count() == 1);
				CHECK(secondary_poses[variant]->get_local_pose(1).origin.is_zero_approx());
			} else {
				CHECK(secondary[variant + 1]->get_bone_pose(1).is_equal_approx(secondary[0]->get_bone_pose(1)));
				CHECK_FALSE(secondary[variant + 1]->get_bone_pose_position(1).is_zero_approx());
			}
		}
		const int64_t skips = method_statistic(fast.processor, "sample_certified_zero_weight_calls");
		if (step == 2) {
			CHECK(skips > 0);
		} else {
			CHECK(skips == 0);
		}
		CHECK(method_statistic(legacy.processor, "sample_certified_zero_weight_calls") == 0);
		CHECK(method_statistic(fast.processor, "zero_weight_mask_rebuilds") == int64_t(step != 1));
		CHECK(method_statistic(legacy.processor, "evaluate_instance_track_skips") == 0);
		CHECK(method_statistic(fast.processor, "evaluate_instance_track_skips") > 0);
		CHECK(method_statistic(legacy.processor, "publish_instance_track_skips") == 0);
		CHECK(method_statistic(fast.processor, "publish_instance_track_skips") == 0);
		CHECK(method_statistic(fast.processor, "publish_eventless_calls") == 1);
		CHECK(method_statistic(fast.processor, "evaluate_owned_weight_copy_elements") == 0);
		for (const char *key : { "sample_pose_instance_calls", "sample_zero_bone_weight_calls", "sample_no_written_bone_calls", "sample_empty_binding_calls" }) {
			CHECK(method_statistic(legacy.processor, key) == method_statistic(fast.processor, key));
		}
	}
}

TEST_CASE("[SceneTree][CZAnimation] Eventless borrowed Tree weights preserve pose auxiliary caches and publication") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(false);
	Rig owned;
	environment.set_enabled(true);
	Rig borrowed;
	Rig synchronous;
	synchronous.processor->unregister_tree(synchronous.handle);
	AuxiliaryTracks owned_aux(owned);
	AuxiliaryTracks borrowed_aux(borrowed);
	AuxiliaryTracks synchronous_aux(synchronous);
	for (Rig *rig : { &owned, &borrowed, &synchronous }) {
		// AuxiliaryTracks adds its method last; keep actual value and BlendShape
		// tracks but remove the event, rather than weakening safe-method checks.
		const int method = rig->animation->get_track_count() - 1;
		REQUIRE(rig->animation->track_get_type(method) == Animation::TYPE_METHOD);
		rig->animation->remove_track(method);
		rig->tree->set_deterministic(true);
		install_zero_certificate_graph(*rig);
	}
	for (int step = 0; step < 4; step++) {
		const NodePath root_path = step == 2 ? NodePath("Skeleton3D:Root") : NodePath();
		for (Rig *rig : { &owned, &borrowed, &synchronous }) {
			rig->tree->set_root_motion_track(root_path);
		}
		synchronous.tree->advance(0.1);
		for (Rig *rig : { &owned, &borrowed }) {
			const int prior = rig->observer->count;
			rig->submit();
			rig->processor->complete();
			CHECK(rig->observer->count == prior);
			rig->processor->publish(rig->handle);
			rig->processor->publish(rig->handle);
			rig->processor->complete_and_publish();
			CHECK(rig->observer->count == prior + 1);
			CHECK(rig->processor->was_evaluated(rig->handle));
			for (int bone = 0; bone < 2; bone++) {
				CHECK(rig->skeleton->get_bone_pose(bone).is_equal_approx(synchronous.skeleton->get_bone_pose(bone)));
			}
			CHECK(rig->tree->get_root_motion_position().is_equal_approx(synchronous.tree->get_root_motion_position()));
			CHECK(rig->tree->get_root_motion_position_accumulator().is_equal_approx(synchronous.tree->get_root_motion_position_accumulator()));
			const bool uses_borrowed = rig == &borrowed;
			CHECK(method_statistic(rig->processor, "evaluate_borrowed_weight_calls") == int64_t(uses_borrowed));
			CHECK(method_statistic(rig->processor, "publish_eventless_calls") == int64_t(uses_borrowed));
			CHECK((method_statistic(rig->processor, "evaluate_owned_weight_copy_elements") > 0) == !uses_borrowed);
			CHECK((method_statistic(rig->processor, "evaluate_borrowed_weight_elements") > 0) == uses_borrowed);
			CHECK(method_statistic(rig->processor, "evaluate_normalized_copy_elements") == (uses_borrowed ? 0 : method_statistic(rig->processor, "evaluate_raw_weight_elements")));
			CHECK(method_statistic(rig->processor, "publish_instance_track_skips") == 0);
		}
		CHECK(method_statistic(owned.processor, "evaluate_raw_weight_elements") == method_statistic(borrowed.processor, "evaluate_raw_weight_elements"));
		for (AuxiliaryTracks *aux : { &owned_aux, &borrowed_aux }) {
			CHECK(aux->target->get_position().is_equal_approx(synchronous_aux.target->get_position()));
			CHECK(Math::is_equal_approx(aux->mesh_instance->get_blend_shape_value(0), synchronous_aux.mesh_instance->get_blend_shape_value(0)));
		}
		if (step == 0) {
			CHECK(method_statistic(borrowed.processor, "evaluate_zero_weight_elements_inspected") > 0);
			CHECK(method_statistic(borrowed.processor, "sample_certified_zero_weight_calls") > 0);
		}
	}
}

TEST_CASE("[SceneTree][CZAnimation] Unused method traits rebuild without denying unrelated borrowed instances") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(true);
	Rig rig(Ref<AnimationBatchProcessor>(), { "rotate_y" });
	rig.tree->set_deterministic(true);
	Ref<Animation> inactive = make_method_animation("rotate_y");
	Ref<AnimationLibrary> library = rig.tree->get_animation_library(StringName());
	REQUIRE(library->add_animation("Inactive", inactive) == OK);
	for (int step = 0; step < 6; step++) {
		if (step == 1) {
			inactive->track_set_enabled(0, false);
		} else if (step == 2) {
			inactive->track_set_enabled(0, true);
		} else if (step == 3) {
			library->rename_animation("Inactive", "Renamed");
		} else if (step == 4) {
			library->remove_animation("Renamed");
		} else if (step == 5) {
			inactive->track_remove_key(0, 0);
			REQUIRE(library->add_animation("EmptyEvent", inactive) == OK);
		}
		rig.submit();
		rig.processor->complete_and_publish();
		CHECK(rig.processor->was_evaluated(rig.handle));
		CHECK(rig.observer->count == step + 1);
		const bool uses_borrowed = true;
		CHECK(method_statistic(rig.processor, "evaluate_borrowed_weight_calls") == int64_t(uses_borrowed));
		CHECK(method_statistic(rig.processor, "publish_eventless_calls") == int64_t(uses_borrowed));
		CHECK((method_statistic(rig.processor, "evaluate_owned_weight_copy_elements") > 0) == !uses_borrowed);
		CHECK(method_statistic(rig.processor, "method_validation_scans") == 1);
		CHECK(method_statistic(rig.processor, "evaluate_eventful_instances") == 0);
		CHECK(method_statistic(rig.processor, "evaluate_unknown_event_traits") == 0);
	}
}

class EventlessGraphReplacement : public RefCounted {
	GDCLASS(EventlessGraphReplacement, RefCounted);

protected:
	static void _bind_methods() {}

public:
	AnimationTree *tree = nullptr;
	Ref<AnimationNodeBlendTree> replacement;
	int calls = 0;
	void replace() {
		calls++;
		tree->set_root_animation_node(replacement);
	}
};

static Ref<AnimationNodeBlendTree> eventless_still_graph(const StringName &p_animation = "Still", const StringName &p_node_name = "Still") {
	Ref<AnimationNodeBlendTree> graph;
	graph.instantiate();
	Ref<AnimationNodeAnimation> clip;
	clip.instantiate();
	clip->set_animation(p_animation);
	graph->add_node(p_node_name, clip);
	graph->connect_node("output", 0, p_node_name);
	return graph;
}

TEST_CASE("[SceneTree][CZAnimation] Actual Walk and Work instances switch borrowing while cues remain publication only") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(true);
	Rig rig(Ref<AnimationBatchProcessor>(), { "rotate_y" });
	rig.tree->set_deterministic(true);
	rig.tree->set_callback_mode_method(AnimationMixer::ANIMATION_CALLBACK_MODE_METHOD_IMMEDIATE);
	Node3D *cue_target = memnew(Node3D);
	cue_target->set_name("CueTarget");
	rig.root->add_child(cue_target);
	Ref<Animation> work = make_method_animation("rotate_y");
	work->track_set_path(0, NodePath("CueTarget"));
	Ref<AnimationLibrary> library = rig.tree->get_animation_library(StringName());
	REQUIRE(library->add_animation("Work", work) == OK);
	int step_index = 0;
	const auto step = [&](const StringName &p_clip, bool p_borrow, real_t p_expected_rotation) {
		// Fresh parameter paths give this independent clip a real zero clock;
		// changing tree_root alone deliberately retains matching parameters.
		rig.tree->set_root_animation_node(eventless_still_graph(p_clip, StringName("Step" + itos(step_index++))));
		rig.tree->advance(0);
		const int before = rig.observer->count;
		const Vector3 rotation_before = cue_target->get_rotation();
		rig.submit(0.6);
		rig.processor->complete();
		CHECK(cue_target->get_rotation().is_equal_approx(rotation_before));
		CHECK(rig.observer->count == before);
		rig.processor->publish(rig.handle);
		rig.processor->publish(rig.handle);
		rig.processor->complete_and_publish();
		CHECK(rig.processor->was_evaluated(rig.handle));
		CHECK(rig.observer->count == before + 1);
		CHECK(Math::is_equal_approx(cue_target->get_rotation().y, p_expected_rotation));
		CHECK(method_statistic(rig.processor, "evaluate_borrowed_weight_calls") == int64_t(p_borrow));
		CHECK(method_statistic(rig.processor, "publish_eventless_calls") == int64_t(p_borrow));
		CHECK((method_statistic(rig.processor, "evaluate_owned_weight_copy_elements") > 0) == !p_borrow);
		CHECK(method_statistic(rig.processor, "evaluate_eventful_instances") == int64_t(!p_borrow));
		CHECK(method_statistic(rig.processor, "evaluate_unknown_event_traits") == 0);
	};
	step("Move", true, 0);
	step("Work", false, 0.125);
	step("Move", true, 0.125);
	// A mutation to a library clip not used in the current frame still drains
	// its borrowed reader before writing and expires the per-animation traits.
	for (bool enabled : { false, true }) {
		rig.submit(0.1);
		rig.processor->complete();
		const int before = rig.observer->count;
		work->track_set_enabled(0, enabled);
		CHECK(rig.observer->count == before + 1);
		rig.processor->complete_and_publish();
		CHECK(method_statistic(rig.processor, "publish_eventless_calls") == 1);
		CHECK(method_statistic(rig.processor, "evaluate_owned_weight_copy_elements") == 0);
	}
	step("Work", false, 0.25);
}

TEST_CASE("[SceneTree][CZAnimation] Eventless publication ends borrowed lifetime before graph replacement callouts") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(true);
	Rig rig;
	rig.tree->set_deterministic(true);
	Ref<EventlessGraphReplacement> action;
	action.instantiate();
	action->tree = rig.tree;
	action->replacement = eventless_still_graph();
	rig.tree->connect(SNAME("mixer_applied"), callable_mp(action.ptr(), &EventlessGraphReplacement::replace), Object::CONNECT_ONE_SHOT);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(action->calls == 1);
	CHECK(rig.observer->count == 1);
	CHECK(method_statistic(rig.processor, "publish_eventless_calls") == 1);
	CHECK(method_statistic(rig.processor, "evaluate_owned_weight_copy_elements") == 0);
	// The old graph's instance buffers can now be freed/rebuilt. A later frame
	// must prepare the replacement, with no pointer back into the old graph.
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->was_evaluated(rig.handle));
	CHECK(rig.observer->count == 2);
	CHECK(rig.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(4, 0, 0)));
	CHECK(method_statistic(rig.processor, "evaluate_borrowed_weight_calls") == 1);
	CHECK(method_statistic(rig.processor, "publish_eventless_calls") == 1);
}

TEST_CASE("[SceneTree][CZAnimation] Supported graph writer publishes borrowed frame before replacing graph storage") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(true);
	Rig rig;
	rig.tree->set_deterministic(true);
	rig.submit();
	rig.processor->complete();
	CHECK(rig.observer->count == 0);
	rig.tree->set_root_animation_node(eventless_still_graph());
	CHECK(rig.observer->count == 1);
	rig.processor->complete_and_publish();
	CHECK(rig.observer->count == 1);
	CHECK(method_statistic(rig.processor, "publish_eventless_calls") == 1);
	rig.submit();
	rig.processor->complete_and_publish();
	CHECK(rig.processor->was_evaluated(rig.handle));
	CHECK(rig.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(4, 0, 0)));
}

TEST_CASE("[SceneTree][CZAnimation] Actual audio instances own snapshots while unused audio permits borrowing") {
	struct AudioFixture {
		AudioDriverDummy *driver = AudioDriverDummy::get_dummy_singleton();
		AudioFixture() {
			driver->set_use_threads(false);
			AudioDriverManager::initialize(AudioDriverManager::get_driver_count() - 1);
			AudioServer *server = memnew(AudioServer);
			server->init();
		}
		~AudioFixture() {
			// Match the existing native audio fixture: consume deferred stream
			// fade-outs after the nodes die, before test-harness server teardown.
			Vector<int32_t> output;
			output.resize(8192 * driver->get_channels());
			driver->mix_audio(8192, output.ptrw());
			AudioServer::get_singleton()->update();
			driver->set_use_threads(true);
		}
	} audio_fixture;
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(true);
	Rig rig;
	rig.tree->set_deterministic(true);
	AudioStreamPlayer *audio = memnew(AudioStreamPlayer);
	audio->set_name("Audio");
	rig.root->add_child(audio);
	Ref<Animation> inactive;
	inactive.instantiate();
	const int track = inactive->add_track(Animation::TYPE_AUDIO);
	inactive->track_set_path(track, NodePath("Audio"));
	Ref<AnimationLibrary> library = rig.tree->get_animation_library(StringName());
	REQUIRE(library->add_animation("InactiveAudio", inactive) == OK);
	for (int step = 0; step < 5; step++) {
		if (step == 1 || step == 4) {
			inactive->track_set_enabled(track, false);
		} else if (step == 2) {
			inactive->track_set_enabled(track, true);
		} else if (step == 3) {
			library->remove_animation("InactiveAudio");
		}
		if (step == 4) {
			REQUIRE(library->add_animation("InactiveAudio", inactive) == OK);
			rig.tree->set_deterministic(false);
		}
		rig.submit();
		rig.processor->complete_and_publish();
		CHECK(rig.processor->was_evaluated(rig.handle));
		CHECK(rig.observer->count == step + 1);
		CHECK(rig.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(step + 1, 0, 0)));
		const bool uses_borrowed = step != 4;
		CHECK(method_statistic(rig.processor, "evaluate_borrowed_weight_calls") == int64_t(uses_borrowed));
		CHECK(method_statistic(rig.processor, "publish_eventless_calls") == int64_t(uses_borrowed));
		CHECK((method_statistic(rig.processor, "evaluate_owned_weight_copy_elements") > 0) == !uses_borrowed);
		CHECK_FALSE(audio->is_playing());
	}
	Rig synchronous;
	Rig batched;
	synchronous.processor->unregister_tree(synchronous.handle);
	AudioStreamPlayer *players[2] = {};
	Rig *rigs[] = { &synchronous, &batched };
	for (int index = 0; index < 2; index++) {
		Rig &current = *rigs[index];
		players[index] = memnew(AudioStreamPlayer);
		players[index]->set_name("Audio");
		current.root->add_child(players[index]);
		Ref<AudioStreamWAV> stream;
		stream.instantiate();
		stream->set_format(AudioStreamWAV::FORMAT_16_BITS);
		stream->set_mix_rate(8000);
		Vector<uint8_t> silence;
		silence.resize(16000);
		silence.fill(0);
		stream->set_data(silence);
		Ref<Animation> clip;
		clip.instantiate();
		const int audio_track = clip->add_track(Animation::TYPE_AUDIO);
		clip->track_set_path(audio_track, NodePath("Audio"));
		clip->audio_track_insert_key(audio_track, 0.2, stream);
		REQUIRE(current.tree->get_animation_library(StringName())->add_animation("AudioClip", clip) == OK);
		current.tree->set_root_animation_node(eventless_still_graph("AudioClip"));
		current.tree->advance(0);
	}
	const int before = batched.observer->count;
	synchronous.tree->advance(0.25);
	batched.submit(0.25);
	batched.processor->complete();
	CHECK_FALSE(players[1]->is_playing());
	CHECK(batched.observer->count == before);
	batched.processor->publish(batched.handle);
	batched.processor->publish(batched.handle);
	batched.processor->complete_and_publish();
	CHECK(players[0]->is_playing());
	CHECK(players[1]->is_playing() == players[0]->is_playing());
	CHECK(batched.observer->count == before + 1);
	CHECK(method_statistic(batched.processor, "evaluate_borrowed_weight_calls") == 0);
	CHECK(method_statistic(batched.processor, "publish_eventless_calls") == 0);
	CHECK(method_statistic(batched.processor, "evaluate_owned_weight_copy_elements") > 0);
}

TEST_CASE("[SceneTree][CZAnimation] Dirty callback generations discard and reevaluate eventless weights before publication") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	for (bool fastpaths : { false, true }) {
		PoseFastpathsEnvironment environment(fastpaths);
		Rig first;
		Rig second;
		Ref<ReentryAction> action;
		action.instantiate();
		action->processor = second.processor;
		action->handles.push_back(second.handle);
		first.tree->connect(SNAME("mixer_applied"), callable_mp(action.ptr(), &ReentryAction::submit), Object::CONNECT_ONE_SHOT);
		first.submit();
		// The callback's later submission is joined but deferred to the next
		// publication epoch, with its output marked dirty by the writer barrier.
		CZAnimationBridge::synchronize();
		CHECK(action->calls == 1);
		CHECK(second.processor->is_pending());
		CHECK(second.observer->count == 0);
		second.processor->complete_and_publish();
		CHECK(second.processor->was_evaluated(second.handle));
		CHECK(second.observer->count == 1);
		CHECK(second.skeleton->get_bone_pose_position(0).is_equal_approx(Vector3(2, 0, 0)));
		CHECK(method_statistic(second.processor, "recomputed") == 1);
		CHECK(method_statistic(second.processor, "evaluate_calls") == 2);
		CHECK(method_statistic(second.processor, "evaluate_borrowed_weight_calls") == (fastpaths ? 2 : 0));
		CHECK(method_statistic(second.processor, "publish_eventless_calls") == int64_t(fastpaths));
		CHECK((method_statistic(second.processor, "evaluate_owned_weight_copy_elements") > 0) == !fastpaths);
	}
}

#ifdef THREADS_ENABLED
TEST_CASE("[SceneTree][CZAnimation] Worker eventless Trees keep queries unpublished and own event-bearing library snapshots") {
	REQUIRE(WorkerThreadPool::get_singleton()->get_thread_count() >= 2);
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(true);
	Ref<AnimationBatchProcessor> processor;
	processor.instantiate();
	struct Rigs {
		LocalVector<Rig *> items;
		~Rigs() {
			for (Rig *rig : items) {
				memdelete(rig);
			}
		}
	} rigs;
	for (int index = 0; index < 16; index++) {
		Rig *rig = memnew(Rig(processor));
		rigs.items.push_back(rig);
		rig->tree->set_deterministic(true);
		if (index == 0) {
			Ref<Animation> event;
			event.instantiate();
			const int track = event->add_track(Animation::TYPE_METHOD);
			event->track_set_path(track, NodePath("."));
			REQUIRE(rig->tree->get_animation_library(StringName())->add_animation("InactiveEmptyEvent", event) == OK);
			Ref<AnimationNodeBlendTree> graph;
			graph.instantiate();
			Ref<AnimationNodeAnimation> event_node;
			event_node.instantiate();
			event_node->set_animation("InactiveEmptyEvent");
			Ref<AnimationNodeBlend2> mix;
			mix.instantiate();
			mix->set_use_sync(true);
			graph->add_node("Move", rig->animation_node);
			graph->add_node("Event", event_node);
			graph->add_node("Mix", mix);
			graph->connect_node("Mix", 0, "Move");
			graph->connect_node("Mix", 1, "Event");
			graph->connect_node("output", 0, "Mix");
			rig->tree->set_root_animation_node(graph);
			rig->tree->set("parameters/Mix/blend_amount", 0.0);
		}
		REQUIRE(processor->queue_update(rig->handle, 0.1));
	}
	CHECK(processor->submit(1, 2) != 0);
	processor->complete();
	for (const Rig *rig : rigs.items) {
		CHECK(rig->pose->get_local_pose(0).origin.is_equal_approx(Vector3(1, 0, 0)));
		CHECK(rig->observer->count == 0);
	}
	processor->complete_and_publish();
	CHECK(method_statistic(processor, "workers") == 2);
	CHECK(method_statistic(processor, "failed") == 0);
	CHECK(method_statistic(processor, "fallbacks") == 0);
	CHECK(method_statistic(processor, "evaluate_calls") == 16);
	CHECK(method_statistic(processor, "evaluate_borrowed_weight_calls") == 15);
	CHECK(method_statistic(processor, "publish_eventless_calls") == 15);
	CHECK(method_statistic(processor, "evaluate_eventful_instances") == 1);
	CHECK(method_statistic(processor, "evaluate_unknown_event_traits") == 0);
	CHECK(method_statistic(processor, "evaluate_owned_weight_copy_elements") > 0);
	CHECK(method_statistic(processor, "evaluate_borrowed_weight_elements") > 0);
	CHECK(method_statistic(processor, "evaluate_raw_weight_elements") == method_statistic(processor, "evaluate_owned_weight_copy_elements") + method_statistic(processor, "evaluate_borrowed_weight_elements"));
	for (const Rig *rig : rigs.items) {
		CHECK(rig->observer->count == 1);
	}
}
#endif

TEST_CASE("[SceneTree][CZAnimation] Inconsistent external graph advance fails borrowed publication without snapshot recovery") {
	PhaseDiagnosticsEnvironment diagnostics(true);
	PoseFastpathsEnvironment environment(true);
	Rig rig;
	rig.tree->set_deterministic(true);
	rig.submit();
	rig.processor->complete();
	// Deliberately bypass the registered producer's ownership with a second
	// direct Advance. The process-pass guard must fail, never read missing
	// snapshots or claim this unsupported double advance as a valid frame.
	rig.tree->advance(0.1);
	CHECK(rig.observer->count == 1);
	rig.processor->complete_and_publish();
	CHECK(rig.observer->count == 1);
	CHECK(method_statistic(rig.processor, "failed") == 1);
	CHECK(method_statistic(rig.processor, "publish_eventless_calls") == 0);
	CHECK(method_statistic(rig.processor, "evaluate_owned_weight_copy_elements") == 0);
}

TEST_CASE("[SceneTree][CZAnimation] Prepared BlendSpace map serves only real 1D and 2D graph nodes") {
	for (bool two_dimensional : { false, true }) {
		Rig original;
		original.processor->unregister_tree(original.handle);
		Rig batched;
		for (Rig *rig : { &original, &batched }) {
			Ref<AnimationNodeBlendTree> graph;
			graph.instantiate();
			Ref<AnimationNodeAnimation> move;
			move.instantiate();
			move->set_animation("Move");
			Ref<AnimationNodeAnimation> still;
			still.instantiate();
			still->set_animation("Still");
			if (two_dimensional) {
				Ref<AnimationNodeBlendSpace2D> space;
				space.instantiate();
				space->set_auto_triangles(false);
				space->add_blend_point(move, Vector2(), -1, "MoveOrigin");
				space->add_blend_point(still, Vector2(1, 0), -1, "StillX");
				Ref<AnimationNodeAnimation> third;
				third.instantiate();
				third->set_animation("Move");
				space->add_blend_point(third, Vector2(0, 1), -1, "MoveY");
				space->add_triangle(0, 1, 2);
				space->set_sample_weight_interpolation_speed(2);
				graph->add_node("Space", space);
			} else {
				Ref<AnimationNodeBlendSpace1D> space;
				space.instantiate();
				space->add_blend_point(move, 0, -1, "MoveZero");
				space->add_blend_point(still, 1, -1, "StillOne");
				space->set_sample_weight_interpolation_speed(2);
				graph->add_node("Space", space);
			}
			graph->connect_node("output", 0, "Space");
			rig->tree->set_root_animation_node(graph);
		}
		for (int step = 0; step < 4; step++) {
			const real_t position = step % 2;
			for (Rig *rig : { &original, &batched }) {
				rig->tree->set("parameters/Space/blend_position", two_dimensional ? Variant(Vector2(position, 0)) : Variant(position));
			}
			original.tree->advance(0.1);
			batched.submit();
			batched.processor->complete();
			CHECK(batched.observer->count == step);
			batched.processor->complete_and_publish();
			CHECK(batched.processor->was_evaluated(batched.handle));
			CHECK(batched.observer->count == step + 1);
			CHECK(batched.skeleton->get_bone_pose(0).is_equal_approx(original.skeleton->get_bone_pose(0)));
		}
	}
}

} // namespace TestCZAnimationRuntime
