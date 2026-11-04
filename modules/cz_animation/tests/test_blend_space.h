/**************************************************************************/
/*  test_blend_space.h                                                    */
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

// Proposal for modules/cz_animation/tests, directly collected by
// modules_tests.gen.h. Migrated cz additions from ee9b55a7; the original
// official create/rename test is excluded.
#include "scene/3d/node_3d.h"
#include "scene/animation/animation_blend_space_1d.h"
#include "scene/animation/animation_blend_space_2d.h"
#include "scene/animation/animation_blend_tree.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/audio/audio_stream_wav.h"
#include "servers/audio/audio_driver_dummy.h"
#include "servers/audio/audio_server.h"
#include "tests/test_macros.h"

#include "modules/cz_animation/animation_batch_processor.h"
#include "modules/cz_animation/animation_bridge.h"

#include <initializer_list>

namespace CZTestAnimationBlendSpace {

// The old all_track_weights_zero flag was an optimization cache, not a new
// AnimationNodeInstance contract. Assert the observable buffer contents
// instead.
static bool all_weights_zero(const AnimationNodeInstance &p_instance) {
	for (real_t weight : p_instance.track_weights) {
		if (weight != 0) {
			return false;
		}
	}
	return true;
}

class TrackWeightProbeNode : public AnimationNode {
	GDCLASS(TrackWeightProbeNode, AnimationNode);

protected:
	static void _bind_methods() {}

	NodeTimeInfo _process(ProcessState &, AnimationNodeInstance &p_instance,
			const AnimationMixer::PlaybackInfo &p_playback_info,
			bool) override {
		process_count++;
		last_seeked = p_playback_info.seeked;
		last_time = p_playback_info.time;
		last_delta = p_playback_info.delta;
		observed_weights.clear();
		for (real_t weight : p_instance.track_weights) {
			observed_weights.push_back(weight);
		}

		NodeTimeInfo info;
		info.length = 10.0;
		info.position = p_playback_info.time;
		info.delta = p_playback_info.delta;
		return info;
	}

public:
	int process_count = 0;
	bool last_seeked = false;
	double last_time = 0.0;
	double last_delta = 0.0;
	LocalVector<real_t> observed_weights;
};

class TrackWeightFilterNode : public AnimationNode {
	GDCLASS(TrackWeightFilterNode, AnimationNode);

protected:
	static void _bind_methods() {}

public:
	bool has_filter() const override { return true; }
};

struct TrackWeightTestRig {
	Ref<TrackWeightFilterNode> filter;
	Ref<TrackWeightProbeNode> probe;
	AnimationNodeInstance source;
	AnimationNodeInstance destination;
	AnimationNode::ProcessState process_state;
	AHashMap<NodePath, int> track_map;
	Variant length = 10.0;
	Variant position = 0.0;
	Variant delta = 0.0;

	TrackWeightTestRig() {
		filter.instantiate();
		probe.instantiate();
		destination.resource = probe;
		destination.parameter_ptrs_by_slot.resize_initialized(
				AnimationNodeInstance::SLOT_MAX);
		destination
				.parameter_ptrs_by_slot[AnimationNodeInstance::SLOT_CURRENT_LENGTH] =
				&length;
		destination
				.parameter_ptrs_by_slot[AnimationNodeInstance::SLOT_CURRENT_POSITION] =
				&position;
		destination
				.parameter_ptrs_by_slot[AnimationNodeInstance::SLOT_CURRENT_DELTA] =
				&delta;

		track_map[NodePath("filtered")] = 0;
		track_map[NodePath("other")] = 1;
		track_map[NodePath("third")] = 2;
		process_state.track_map = &track_map;
		process_state.track_map_updated = true;
		AnimationNode::tls_process_state = &process_state;
	}

	~TrackWeightTestRig() { AnimationNode::tls_process_state = nullptr; }

	void set_source_weights(std::initializer_list<real_t> p_weights) {
		source.track_weights.clear();
		for (real_t weight : p_weights) {
			source.track_weights.push_back(weight);
		}
	}

	AnimationNode::NodeTimeInfo
	blend(real_t p_weight,
			AnimationNode::FilterAction p_filter = AnimationNode::FILTER_IGNORE,
			bool p_sync = true, bool p_seeked = false, double p_time = 0.0,
			double p_delta = 0.25, bool p_test_only = false) {
		AnimationMixer::PlaybackInfo playback_info;
		playback_info.weight = p_weight;
		playback_info.seeked = p_seeked;
		playback_info.time = p_time;
		playback_info.delta = p_delta;
		return filter->_blend_node(process_state, source, destination,
				playback_info, p_filter, p_sync, p_test_only);
	}
};

TEST_CASE("[SceneTree][CZAnimation][AnimationTree] Group parameter writes "
		  "match individual writes and reject invalid groups") {
	Ref<AnimationNodeBlendTree> blend_tree;
	blend_tree.instantiate();
	Ref<AnimationNodeBlendSpace2D> lower_space;
	lower_space.instantiate();
	Ref<AnimationNodeBlendSpace2D> upper_space;
	upper_space.instantiate();
	blend_tree->add_node("Lower", lower_space);
	blend_tree->add_node("Upper", upper_space);

	AnimationTree *group_tree = memnew(AnimationTree);
	AnimationTree *individual_tree = memnew(AnimationTree);
	group_tree->set_root_animation_node(blend_tree);
	individual_tree->set_root_animation_node(blend_tree);
	Window *scene_root = SceneTree::get_singleton()->get_root();
	scene_root->add_child(group_tree);
	scene_root->add_child(individual_tree);

	const StringName lower_position("parameters/Lower/blend_position");
	const StringName upper_position("parameters/Upper/blend_position");
	TypedArray<StringName> position_group;
	position_group.push_back(lower_position);
	position_group.push_back(upper_position);
	const Vector2 grouped_value(0.75, -0.25);
	CHECK(AnimationBatchProcessor::set_parameter_group(group_tree, position_group,
			grouped_value));
	CHECK_EQ(group_tree->get(lower_position), Variant(grouped_value));
	CHECK_EQ(group_tree->get(upper_position), Variant(grouped_value));
	CHECK_EQ(
			individual_tree->get(lower_position),
			Variant(Vector2())); // Parameter storage belongs to each tree instance.

	bool valid = false;
	individual_tree->set(lower_position, grouped_value, &valid);
	CHECK(valid);
	individual_tree->set(upper_position, grouped_value, &valid);
	CHECK(valid);
	CHECK_EQ(individual_tree->get(lower_position),
			group_tree->get(lower_position));
	CHECK_EQ(individual_tree->get(upper_position),
			group_tree->get(upper_position));

	const Vector2 before_invalid_group = group_tree->get(lower_position);
	TypedArray<StringName> unknown_group;
	unknown_group.push_back(lower_position);
	unknown_group.push_back("parameters/Missing/blend_position");
	ERR_PRINT_OFF;
	CHECK_FALSE(AnimationBatchProcessor::set_parameter_group(
			group_tree, unknown_group, Vector2(1.0, 1.0)));
	ERR_PRINT_ON;
	CHECK_EQ(
			group_tree->get(lower_position),
			Variant(before_invalid_group)); // Invalid groups do not partially apply.

	TypedArray<StringName> invalid_type_group;
	invalid_type_group.push_back(lower_position);
	invalid_type_group.push_back("parameters/Lower/cycle_duration_override");
	ERR_PRINT_OFF;
	CHECK_FALSE(AnimationBatchProcessor::set_parameter_group(
			group_tree, invalid_type_group, Vector2(1.0, 1.0)));
	ERR_PRINT_ON;
	CHECK_EQ(group_tree->get(lower_position),
			Variant(before_invalid_group)); // A valid first type does not apply
											// before a later mismatch.

	TypedArray<StringName> numeric_group;
	numeric_group.push_back("parameters/Lower/cycle_duration_override");
	CHECK(AnimationBatchProcessor::set_parameter_group(
			group_tree, numeric_group,
			3)); // Keep Object.Set's integer-to-float conversion.
	CHECK_EQ(group_tree->get("parameters/Lower/cycle_duration_override"),
			Variant(3.0));

	TypedArray<StringName> read_only_group;
	read_only_group.push_back(lower_position);
	read_only_group.push_back("parameters/Lower/effective_cycle_duration");
	ERR_PRINT_OFF;
	CHECK_FALSE(AnimationBatchProcessor::set_parameter_group(
			group_tree, read_only_group, Vector2(1.0, 1.0)));
	ERR_PRINT_ON;
	CHECK_EQ(group_tree->get(lower_position), Variant(before_invalid_group));

	scene_root->remove_child(group_tree);
	scene_root->remove_child(individual_tree);
	memdelete(group_tree);
	memdelete(individual_tree);
}

TEST_CASE("[SceneTree][CZAnimation][AnimationBlendTree] Exact zero weights "
		  "clear once and preserve filters across reentry") {
	TrackWeightTestRig rig;
	rig.filter->set_filter_enabled(true);
	rig.filter->set_filter_path(NodePath("filtered"), true);
	rig.process_state.track_map_updated = true;
	rig.set_source_weights({ 0.5, 0.8, 0.25 });

	rig.blend(0.5, AnimationNode::FILTER_PASS);
	CHECK_EQ(rig.probe->observed_weights.size(), 3);
	CHECK_EQ(rig.probe->observed_weights[0], 0.25);
	CHECK_EQ(rig.probe->observed_weights[1], 0.0);
	CHECK_EQ(rig.probe->observed_weights[2], 0.0);
	CHECK_FALSE(all_weights_zero(rig.destination));
	CHECK(rig.destination.is_blended());
	CHECK_EQ(rig.source.filtered_track_indices_cache.size(), 1);
	CHECK_EQ(rig.source.filtered_track_indices_cache[0], 0);

	// A zero parent clears stale destination weights, then reuses the known-zero
	// buffer.
	rig.process_state.track_map_updated = false;
	rig.set_source_weights({ 0.0, 0.0, 0.0 });
	rig.blend(0.5, AnimationNode::FILTER_PASS, true, false, 0.0, 0.25);
	CHECK(all_weights_zero(rig.destination));
	CHECK_FALSE(rig.destination.is_blended());
	CHECK_EQ(rig.destination.track_weights[0], 0.0);
	CHECK_EQ(rig.destination.track_weights[1], 0.0);
	CHECK_EQ(rig.destination.track_weights[2], 0.0);
	CHECK_EQ(rig.probe->process_count, 2);
	CHECK_EQ(rig.probe->last_delta, 0.25);
	CHECK_EQ((double)rig.position, 0.5);

	// FILTER_STOP deliberately keeps filtered tracks at full weight under a zero
	// parent.
	rig.blend(0.5, AnimationNode::FILTER_STOP);
	CHECK_FALSE(all_weights_zero(rig.destination));
	CHECK_FALSE(
			rig.destination.is_blended()); // FILTER_STOP excludes the filtered track
										   // from child blending.
	CHECK_EQ(rig.probe->observed_weights[0], 1.0);
	CHECK_EQ(rig.probe->observed_weights[1], 0.0);
	CHECK_EQ(rig.probe->process_count, 3);

	// The skipped filter cache can remain stale while weights are zero; it must
	// refresh on reentry.
	rig.blend(0.5, AnimationNode::FILTER_PASS);
	CHECK(all_weights_zero(rig.destination));
	rig.filter->set_filter_path(NodePath("filtered"), false);
	rig.filter->set_filter_path(NodePath("other"), true);
	rig.blend(0.5, AnimationNode::FILTER_BLEND);
	CHECK(all_weights_zero(rig.destination));
	// The official implementation eagerly refreshes this cache at zero weight.
	// The old optimization permitted it to remain stale until nonzero reentry;
	// only the following nonzero output assertions are part of the contract.
#if 0 // Legacy optimization introspection; not a requirement for the new
	  // bridge.
	CHECK_EQ(rig.source.filtered_track_indices_cache[0], 0);
#endif
	CHECK_EQ(rig.probe->process_count, 5);

	rig.set_source_weights({ 0.5, 0.8, 0.25 });
	rig.blend(0.5, AnimationNode::FILTER_BLEND);
	CHECK_FALSE(all_weights_zero(rig.destination));
	CHECK_EQ(rig.source.filtered_track_indices_cache[0], 1);
	CHECK_EQ(rig.probe->observed_weights[0],
			0.5); // Unfiltered values pass without local blending.
	CHECK(Math::is_equal_approx(
			(double)rig.probe->observed_weights[1],
			0.4)); // Filtered values use the local blend weight.
	CHECK_EQ(rig.probe->observed_weights[2], 0.25);
	CHECK_EQ(rig.probe->process_count, 6);

	// Resizing invalidates the cached state; a zero input clears the newly sized
	// buffer.
	rig.set_source_weights({ 0.5, 0.25 });
	rig.blend(0.5);
	CHECK_EQ(rig.destination.track_weights.size(), 2);
	CHECK_FALSE(all_weights_zero(rig.destination));
	rig.set_source_weights({ 0.0, 0.0, 0.0, 0.0 });
	rig.blend(0.5);
	CHECK_EQ(rig.destination.track_weights.size(), 4);
	CHECK(all_weights_zero(rig.destination));
	for (real_t weight : rig.destination.track_weights) {
		CHECK_EQ(weight, 0.0);
	}

	AnimationNodeInstance recreated_instance;
	CHECK(all_weights_zero(recreated_instance)); // New instances start with an
												 // empty, zero-weight vector.
}

TEST_CASE("[SceneTree][CZAnimation][AnimationBlendTree] Exact zero path "
		  "distinguishes tiny weights and preserves child processing") {
	TrackWeightTestRig rig;
	rig.filter->set_filter_enabled(false);
	rig.set_source_weights({ CMP_EPSILON * 0.5 });
	CHECK_FALSE(all_weights_zero(rig.source));

	// This value is approximately zero before blending but becomes significant
	// when amplified.
	rig.blend(4.0);
	CHECK_EQ(rig.probe->observed_weights.size(), 1);
	CHECK(Math::is_equal_approx((double)rig.probe->observed_weights[0],
			(double)CMP_EPSILON * 2.0));
	CHECK_FALSE(all_weights_zero(rig.destination));
	CHECK(rig.destination.is_blended());

	// Non-finite local weights stay on the original path (0 * infinity is NaN).
	rig.set_source_weights({ 0.0 });
	rig.blend(Math::INF);
	CHECK(Math::is_nan(rig.probe->observed_weights[0]));
	CHECK_FALSE(all_weights_zero(rig.destination));

	// The all-zero child is still preprocessed, preserving synchronized clock and
	// seek behavior.
	rig.set_source_weights({ 0.0 });
	rig.position = 0.0;
	rig.blend(1.0, AnimationNode::FILTER_IGNORE, true, false, 0.0, 0.25);
	CHECK_EQ(rig.probe->process_count, 3);
	CHECK_EQ((double)rig.position, 0.25);
	CHECK_EQ(rig.probe->last_delta, 0.25);

	rig.blend(1.0, AnimationNode::FILTER_IGNORE, false, false, 0.0, 0.75);
	CHECK_EQ(
			(double)rig.position,
			0.25); // Unsynced zero-weight edges retain the existing zero-delta rule.
	CHECK_EQ(rig.probe->last_delta, 0.0);

	rig.blend(1.0, AnimationNode::FILTER_IGNORE, false, true, 8.5, 0.0);
	CHECK(rig.probe->last_seeked);
	CHECK_EQ(rig.probe->last_time, 8.5);
	CHECK_EQ((double)rig.position, 8.5);
	CHECK_EQ(rig.probe->process_count, 5);
}

TEST_CASE("[SceneTree][CZAnimation][AnimationBlendSpace] Sample weights "
		  "interpolate per instance and reset on seek") {
	Ref<AnimationNodeBlendSpace1D> blend_space_1d;
	blend_space_1d.instantiate();
	CHECK_EQ(blend_space_1d->get_sample_weight_interpolation_speed(), 0.0f);
	CHECK_EQ(
			blend_space_1d->get_parameter_default_value("cycle_duration_override"),
			Variant(0.0));
	CHECK_EQ(
			blend_space_1d->get_parameter_default_value("effective_cycle_duration"),
			Variant(0.0));
	CHECK_FALSE(
			blend_space_1d->is_parameter_read_only("cycle_duration_override"));
	CHECK(blend_space_1d->is_parameter_read_only("effective_cycle_duration"));
	blend_space_1d->set_sample_weight_interpolation_speed(2.0f);
	CHECK_EQ(blend_space_1d->get_sample_weight_interpolation_speed(), 2.0f);

	Ref<AnimationNodeBlendSpace2D> blend_space_2d;
	blend_space_2d.instantiate();
	CHECK_EQ(blend_space_2d->get_sample_weight_interpolation_speed(), 0.0f);
	CHECK_EQ(
			blend_space_2d->get_parameter_default_value("cycle_duration_override"),
			Variant(0.0));
	CHECK_EQ(
			blend_space_2d->get_parameter_default_value("effective_cycle_duration"),
			Variant(0.0));
	CHECK_FALSE(
			blend_space_2d->is_parameter_read_only("cycle_duration_override"));
	CHECK(blend_space_2d->is_parameter_read_only("effective_cycle_duration"));
	blend_space_2d->set_sample_weight_interpolation_speed(2.0f);
	CHECK_EQ(blend_space_2d->get_sample_weight_interpolation_speed(), 2.0f);

	AnimationTree *first_tree = memnew(AnimationTree);
	AnimationTree *second_tree = memnew(AnimationTree);
	// Sharing the same graph resource must not share interpolation history.
	first_tree->set_root_animation_node(blend_space_1d);
	second_tree->set_root_animation_node(blend_space_1d);
	Window *root = SceneTree::get_singleton()->get_root();
	root->add_child(first_tree);
	root->add_child(second_tree);
	const StringName path("parameters/");
	const auto update = [&](AnimationTree *p_tree,
								const LocalVector<float> &p_target, double p_delta,
								bool p_seek) {
		LocalVector<float> result(p_target);
		CZAnimationBridge::interpolate_blend_space_weights(p_tree, path, result,
				2.0f, p_delta, p_seek);
		return result;
	};
	update(first_tree, { 1, 0 }, 0, false);
	update(second_tree, { 1, 0 }, 0, false);
	LocalVector<float> result = update(first_tree, { 0, 1 }, 0.25, false);
	CHECK_EQ(result[0], 0.5f);
	CHECK_EQ(result[1], 0.5f);
	CHECK_EQ(result[0] + result[1], 1.0f);
	CHECK_EQ(CZAnimationBridge::blend_space(second_tree, path).weights[0], 1.0f);
	CHECK_EQ(CZAnimationBridge::blend_space(second_tree, path).weights[1], 0.0f);
	result = update(first_tree, { 0, 1 }, 0, false);
	CHECK_EQ(result[0], 0.5f);
	CHECK_EQ(result[1], 0.5f);
	CZAnimationBridge::blend_space(first_tree, path).weights[0] = 0.2f;
	CZAnimationBridge::blend_space(first_tree, path).weights[1] = 0.3f;
	result = update(first_tree, { 0, 1 }, 0, false);
	CHECK_EQ(result[0], 0.2f);
	CHECK_EQ(result[1], 0.3f);
	result = update(first_tree, { 0, 1 }, 0, true);
	CHECK_EQ(result[0], 0.0f);
	CHECK_EQ(result[1], 1.0f);
	CZAnimationBridge::invalidate_graph(first_tree);
	result = update(first_tree, { 1, 0 }, 0, false);
	CHECK_EQ(result[0], 1.0f);
	CHECK_EQ(result[1], 0.0f);
	// Explicitly clear histories before deleting trees; object-ID reuse cannot
	// accidentally give a later fixture the current test's interpolation state.
	CZAnimationBridge::invalidate_graph(first_tree);
	CZAnimationBridge::invalidate_graph(second_tree);
	memdelete(first_tree);
	memdelete(second_tree);
}

TEST_CASE("[SceneTree][CZAnimation][AnimationBlendSpace] Cycle duration "
		  "override synchronizes spaces with different clip lengths") {
	LocalVector<float> weights;
	weights.push_back(0.5f);
	weights.push_back(0.5f);
	LocalVector<double> lower_lengths;
	lower_lengths.push_back(1.0);
	lower_lengths.push_back(3.0);
	LocalVector<double> upper_lengths;
	upper_lengths.push_back(2.0);
	upper_lengths.push_back(6.0);
	double lower_effective = 0.0;
	double upper_effective = 0.0;
	const double no_override =
			CZAnimationBridge::resolve_blend_space_cycle_duration(
					weights, lower_lengths, 0.0, lower_effective);
	CHECK_EQ(no_override, 2.0);
	CHECK_EQ(lower_effective, 2.0);
	const double shared_cycle =
			CZAnimationBridge::resolve_blend_space_cycle_duration(
					weights, upper_lengths, lower_effective, upper_effective);
	CHECK_EQ(shared_cycle, lower_effective);
	CHECK_EQ(upper_effective, 4.0);
	CHECK_NE(upper_effective, shared_cycle);
}

// The former transform-only classification and skip-predicate APIs no longer
// exist on official AnimationMixer. Keep their original assertions below as an
// explicit migration record, and test the behavior through real mixed tracks.
struct MixedTrackRig {
	Node3D *root = nullptr;
	Skeleton3D *skeleton = nullptr;
	Node3D *target = nullptr;
	AudioStreamPlayer *audio = nullptr;
	AnimationPlayer *player = nullptr;
	Ref<Animation> animation;

	MixedTrackRig() {
		root = memnew(Node3D);
		skeleton = memnew(Skeleton3D);
		skeleton->set_name("Skeleton");
		skeleton->add_bone("Root");
		skeleton->set_modifier_callback_mode_process(
				Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
		root->add_child(skeleton);
		target = memnew(Node3D);
		target->set_name("Target");
		root->add_child(target);
		audio = memnew(AudioStreamPlayer);
		audio->set_name("Audio");
		root->add_child(audio);
		player = memnew(AnimationPlayer);
		player->set_root_node(NodePath(".."));
		player->set_callback_mode_process(
				AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
		player->set_callback_mode_method(
				AnimationMixer::ANIMATION_CALLBACK_MODE_METHOD_IMMEDIATE);
		root->add_child(player);
		animation.instantiate();
		animation->set_length(1);
		const int track = animation->add_track(Animation::TYPE_POSITION_3D);
		animation->track_set_path(track, NodePath("Skeleton:Root"));
		animation->position_track_insert_key(track, 0, Vector3());
		animation->position_track_insert_key(track, 1, Vector3(1, 0, 0));
		Ref<AnimationLibrary> library;
		library.instantiate();
		library->add_animation("test", animation);
		player->add_animation_library(StringName(), library);
		SceneTree::get_singleton()->get_root()->add_child(root);
		player->play("test");
	}

	void add_non_pose_tracks() {
		int track = animation->add_track(Animation::TYPE_VALUE);
		animation->track_set_path(track, NodePath("Target:position"));
		animation->track_insert_key(track, 0, Vector3());
		animation->track_insert_key(track, 1, Vector3(0, 2, 0));
		track = animation->add_track(Animation::TYPE_METHOD);
		animation->track_set_path(track, NodePath("Target"));
		Dictionary key;
		key["method"] = StringName("rotate_y");
		Array args;
		args.push_back(0.25);
		key["args"] = args;
		animation->track_insert_key(track, 0.2, key);
		Ref<AudioStreamWAV> stream;
		stream.instantiate();
		stream->set_format(AudioStreamWAV::FORMAT_16_BITS);
		stream->set_mix_rate(8000);
		Vector<uint8_t> silence;
		silence.resize(16000);
		silence.fill(0);
		stream->set_data(silence);
		track = animation->add_track(Animation::TYPE_AUDIO);
		animation->track_set_path(track, NodePath("Audio"));
		animation->audio_track_insert_key(track, 0.2, stream);
	}

	~MixedTrackRig() { memdelete(root); }
};

TEST_CASE("[SceneTree][CZAnimation][AnimationMixer] Track mutation preserves "
		  "value method and audio publication") {
	// The standard test harness returns after creating SceneTree, so combining
	// [SceneTree] with [Audio] does not initialize the audio server for this case.
	struct AudioFixture {
		AudioDriverDummy *driver = AudioDriverDummy::get_dummy_singleton();
		AudioFixture() {
			driver->set_use_threads(false);
			AudioDriverManager::initialize(AudioDriverManager::get_driver_count() - 1);
			AudioServer *server = memnew(AudioServer);
			server->init();
		}
		~AudioFixture() {
			// Node destruction requests an audio-thread fade-out. Consume that
			// queue deterministically before the harness shuts AudioServer down.
			Vector<int32_t> output;
			output.resize(8192 * driver->get_channels());
			driver->mix_audio(8192, output.ptrw());
			AudioServer::get_singleton()->update();
			driver->set_use_threads(true);
		}
	} audio_fixture;
	MixedTrackRig synchronous;
	MixedTrackRig batched;
	Ref<SkeletonAnimationPose> pose;
	pose.instantiate();
	Ref<AnimationBatchProcessor> processor;
	processor.instantiate();
	REQUIRE(pose->capture(batched.skeleton));
	const int64_t handle =
			processor->register_player(batched.player, { pose }, { "rotate_y" });
	REQUIRE(handle != 0);
	synchronous.player->advance(0.1);
	REQUIRE(processor->queue_update(handle, 0.1));
	processor->submit(1, 1);
	processor->complete_and_publish();
	REQUIRE(processor->was_evaluated(handle));
	CHECK(batched.skeleton->get_bone_pose_position(0).is_equal_approx(
			synchronous.skeleton->get_bone_pose_position(0)));

	// Mutate the already cached transform-only clip, retaining the registration.
	synchronous.add_non_pose_tracks();
	batched.add_non_pose_tracks();
	synchronous.player->advance(0.25);
	REQUIRE(processor->queue_update(handle, 0.25));
	processor->submit(1, 2);
	processor->complete();
	CHECK(batched.target->get_position().is_zero_approx());
	CHECK(batched.target->get_rotation().is_zero_approx());
	CHECK_FALSE(batched.audio->is_playing());
	processor->complete_and_publish();
	REQUIRE(processor->was_evaluated(handle));
	CHECK(batched.target->get_position().is_equal_approx(
			synchronous.target->get_position()));
	CHECK(Math::is_equal_approx(batched.target->get_rotation().y, real_t(0.25)));
	CHECK(batched.target->get_rotation().is_equal_approx(
			synchronous.target->get_rotation()));
	CHECK(synchronous.audio->is_playing());
	CHECK(batched.audio->is_playing());
	CHECK(batched.skeleton->get_bone_pose_position(0).is_equal_approx(
			synchronous.skeleton->get_bone_pose_position(0)));
	processor->complete_and_publish();
	CHECK(Math::is_equal_approx(batched.target->get_rotation().y,
			real_t(0.25))); // Method ran exactly once.
	processor->unregister_animation(handle);
}

#if 0 // Legacy core-only introspection: these APIs/fields are deliberately
	  // absent.
// all_track_weights_zero used to cache exact-zero buffer state. The active tests
// above compare actual buffers, filter reentry, tiny weights, clocks and seeks.
// Preserve representative old assertions without reintroducing layout fields:
static void legacy_zero_flag_assertions(const AnimationNodeInstance &p_instance) {
	CHECK(p_instance.all_track_weights_zero);
}
class CacheTestAnimationMixer : public AnimationMixer {
	GDCLASS(CacheTestAnimationMixer, AnimationMixer);

protected:
	static void _bind_methods() {}

public:
	void prime_track_cache(const Ref<Animation> &p_animation) {
		_create_track_num_to_track_cache_for_animation(p_animation);
	}
};

TEST_CASE("[SceneTree][CZAnimation][AnimationMixer] Transform-only cache invalidates for method, value, and audio tracks") {
	CacheTestAnimationMixer *mixer = memnew(CacheTestAnimationMixer);
	Ref<Animation> animation;
	animation.instantiate();
	const int position_track = animation->add_track(Animation::TYPE_POSITION_3D);
	const Ref<AnimationLibrary> library = memnew(AnimationLibrary);
	CHECK_EQ(library->add_animation("transform", animation), OK);
	CHECK_EQ(mixer->add_animation_library("", library), OK);
	mixer->prime_track_cache(animation);
	CHECK(mixer->is_animation_transform_only(animation));

	const int method_track = animation->add_track(Animation::TYPE_METHOD);
	CHECK_FALSE(mixer->is_animation_transform_only(animation)); // Track mutation clears the mixer's classification cache.
	mixer->prime_track_cache(animation);
	CHECK_FALSE(mixer->is_animation_transform_only(animation));
	animation->remove_track(method_track);
	mixer->prime_track_cache(animation);
	CHECK(mixer->is_animation_transform_only(animation));

	const int value_track = animation->add_track(Animation::TYPE_VALUE);
	mixer->prime_track_cache(animation);
	CHECK_FALSE(mixer->is_animation_transform_only(animation));
	animation->remove_track(value_track);
	mixer->prime_track_cache(animation);
	CHECK(mixer->is_animation_transform_only(animation));

	const int audio_track = animation->add_track(Animation::TYPE_AUDIO);
	mixer->prime_track_cache(animation);
	CHECK_FALSE(mixer->is_animation_transform_only(animation));
	animation->remove_track(audio_track);
	mixer->prime_track_cache(animation);
	CHECK(mixer->is_animation_transform_only(animation));
	memdelete(mixer);
}

TEST_CASE("[SceneTree][CZAnimation][AnimationMixer] Skip predicate preserves blended and non-pose evaluations") {
	CHECK(AnimationMixer::should_skip_unblended_animation(false, true));
	CHECK_FALSE(AnimationMixer::should_skip_unblended_animation(true, true));
	CHECK_FALSE(AnimationMixer::should_skip_unblended_animation(false, false));
}
#endif // Legacy core-only introspection; mixed-track behavior has an active
	   // replacement.

} // namespace CZTestAnimationBlendSpace
