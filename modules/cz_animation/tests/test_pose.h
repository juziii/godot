/**************************************************************************/
/*  test_pose.h                                                           */
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

#include "core/math/vector2.h"
#include "tests/test_macros.h"

#include "modules/cz_animation/bone_lod.h"
#include "modules/cz_animation/pose_soa.h"

namespace TestCZAnimation {

TEST_CASE("[CZAnimation] SoA scalar and SIMD preserve slerp and padded tails") {
	const int counts[] = { 0, 1, 3, 4, 5, 17 };
	for (int count : counts) {
		CZPoseSoA from(CZPoseSoA::SOA);
		CZPoseSoA to(CZPoseSoA::SOA);
		CZPoseSoA scalar(CZPoseSoA::SOA);
		CZPoseSoA simd(CZPoseSoA::SOA);
		from.resize(count);
		to.resize(count);
		for (int i = 0; i < count; i++) {
			from.set_position(i, Vector3(i, -i, 2 * i));
			to.set_position(i, Vector3(-i, 2 * i, i));
			from.set_scale(i, Vector3(-1, 2, 3));
			to.set_scale(i, Vector3(2, -3, 4));
			from.set_rotation(i, Quaternion(Vector3(0, 1, 0), 0.1 * i));
			to.set_rotation(i, -Quaternion(Vector3(1, 0, 0), 0.3 * i));
		}
		scalar.interpolate(from, to, 0.37, false);
		simd.interpolate(from, to, 0.37, true);
		for (int i = 0; i < count; i++) {
			CHECK(simd.get_position(i).is_equal_approx(scalar.get_position(i)));
			CHECK(simd.get_scale(i).is_equal_approx(scalar.get_scale(i)));
			CHECK(simd.get_rotation(i).is_equal_approx(from.get_rotation(i).slerp(to.get_rotation(i), 0.37)));
		}
		for (int i = count; i < simd.padded_size(); i++) {
			CHECK(simd.data(CZPoseSoA::POSITION_X)[i] == 0);
			CHECK(simd.data(CZPoseSoA::ROTATION_W)[i] == 1);
			CHECK(simd.data(CZPoseSoA::SCALE_X)[i] == 1);
		}
	}
}

TEST_CASE("[CZAnimation] SoA blending follows official rest and quaternion order") {
	CZPoseSoA rest(CZPoseSoA::SOA);
	CZPoseSoA sample(CZPoseSoA::SOA);
	CZPoseSoA scalar(CZPoseSoA::SOA);
	CZPoseSoA simd(CZPoseSoA::SOA);
	rest.resize(5);
	sample.resize(5);
	LocalVector<real_t> weights;
	weights.resize(rest.padded_size());
	for (int i = 0; i < rest.padded_size(); i++) {
		weights[i] = i < rest.size() ? 0.25 : 0;
	}
	for (int i = 0; i < rest.size(); i++) {
		rest.set_position(i, Vector3(1, 2, 3));
		rest.set_rotation(i, Quaternion(Vector3(0, 0, 1), 0.5));
		rest.set_scale(i, Vector3(1, -1, 2));
		sample.set_position(i, Vector3(5, 3, -1));
		sample.set_rotation(i, Quaternion(Vector3(1, 0, 0), 2));
		sample.set_scale(i, Vector3(3, -2, 1));
	}
	scalar.copy_from(rest);
	simd.copy_from(rest);
	for (int instance = 0; instance < 3; instance++) {
		scalar.blend(sample, rest, weights.ptr(), weights.ptr(), weights.ptr(), false);
		simd.blend(sample, rest, weights.ptr(), weights.ptr(), weights.ptr(), true);
	}
	Quaternion expected = rest.get_rotation(0);
	for (int instance = 0; instance < 3; instance++) {
		expected = Animation::interpolate_via_rest(expected, sample.get_rotation(0), 0.25, rest.get_rotation(0));
	}
	for (int i = 0; i < rest.size(); i++) {
		CHECK(scalar.get_rotation(i).is_equal_approx(expected));
		CHECK(simd.get_rotation(i).is_equal_approx(expected));
		CHECK(simd.get_position(i).is_equal_approx(Vector3(4, 2.75, 0)));
		CHECK(simd.get_scale(i).is_equal_approx(scalar.get_scale(i)));
	}
}

TEST_CASE("[CZAnimation] Bone LOD keeps ancestors and expands original palette indices") {
	// The original order intentionally places the root after its descendants.
	LocalVector<int> parents = { 3, 0, 1, -1, 3 };
	LocalVector<int> thresholds = { 0, 1, 1, 0, 1 };
	LocalVector<Transform3D> rest;
	rest.resize(parents.size());
	for (Transform3D &transform : rest) {
		transform.origin = Vector3(0, 1, 0);
	}
	CZBoneLod lod;
	CHECK(lod.build(parents, rest, thresholds, {}, 1));
	CHECK(lod.get_active_bone_count() == 2);
	CHECK(lod.to_compact(1) == -1);
	CZPoseSoA full(CZPoseSoA::SOA);
	full.resize(parents.size());
	for (int i = 0; i < full.size(); i++) {
		full.set_position(i, rest[i].origin);
	}
	full.set_position(3, Vector3(5, 0, 0));
	CZPoseSoA compact(CZPoseSoA::SOA);
	lod.gather(full, compact);
	LocalVector<Transform3D> palette;
	lod.expand_global(compact, palette);
	CHECK(palette.size() == parents.size());
	CHECK(palette[2].origin.is_equal_approx(Vector3(5, 3, 0)));
	CHECK(palette[4].origin.is_equal_approx(Vector3(5, 1, 0)));
	CHECK(lod.build(parents, rest, thresholds, { 2 }, 1));
	CHECK(lod.get_active_bone_count() == 4);
	CHECK(lod.to_compact(0) >= 0);
	CHECK(lod.to_compact(1) >= 0);
	CHECK(lod.build(parents, rest, thresholds, {}, 1, true));
	CHECK(lod.get_active_bone_count() == 5);
	CHECK(lod.build(parents, rest, thresholds, {}, 0));
	CHECK(lod.get_active_bone_count() == 5);
	parents[3] = 2;
	CHECK_FALSE(lod.build(parents, rest, thresholds, {}, 1));
	CHECK(lod.get_active_bone_count() == 5);
}

TEST_CASE("[CZAnimation] Direct track sampling retains missing channels and motion scale") {
	Ref<Animation> animation;
	animation.instantiate();
	const int track = animation->add_track(Animation::TYPE_POSITION_3D);
	animation->position_track_insert_key(track, 0, Vector3(0, 2, 0));
	animation->position_track_insert_key(track, 1, Vector3(0, 4, 0));
	CZPoseSoA rest(CZPoseSoA::SOA);
	rest.resize(3);
	rest.set_position(1, Vector3(0, 1, 0));
	rest.set_scale(1, Vector3(1, 2, -1));
	CZPoseSoA result(CZPoseSoA::SOA);
	result.copy_from(rest);
	CZPoseSoA sample(CZPoseSoA::SOA);
	CZPoseTrackBinding binding;
	binding.reset(animation, 3);
	binding.add_track(Animation::TYPE_POSITION_3D, track, 1, 2);
	LocalVector<real_t> weights = { 0, 0, 0.5 };
	binding.sample_and_blend(0.5, 1, &weights, 2, rest, sample, result, true);
	CHECK(result.get_position(1).is_equal_approx(Vector3(0, 3.5, 0)));
	CHECK(result.get_scale(1).is_equal_approx(rest.get_scale(1)));
	CHECK(result.get_position(0).is_equal_approx(rest.get_position(0)));
	CHECK(result.get_rotation(1).is_equal_approx(rest.get_rotation(1)));
}

TEST_CASE("[CZAnimation] Direct track bindings reject padded indices stale logical sizes and aliasing") {
	Ref<Animation> animation;
	animation.instantiate();
	const int track = animation->add_track(Animation::TYPE_POSITION_3D);
	animation->position_track_insert_key(track, 0, Vector3(10, 0, 0));
	CZPoseTrackBinding binding;
	binding.reset(animation, 3);
	CZPoseSoA rest(CZPoseSoA::SOA);
	CZPoseSoA sample(CZPoseSoA::SOA);
	CZPoseSoA result(CZPoseSoA::SOA);
	rest.resize(3);
	result.copy_from(rest);
	ERR_PRINT_OFF;
	binding.add_track(Animation::TYPE_POSITION_3D, track, 3, 0);
	binding.add_track(Animation::TYPE_ROTATION_3D, track, 1, 0);
	ERR_PRINT_ON;
	binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, true);
	for (int bone = 0; bone < result.size(); bone++) {
		CHECK(result.get_position(bone).is_zero_approx());
	}
	binding.add_track(Animation::TYPE_POSITION_3D, track, 1, 0);
	ERR_PRINT_OFF;
	binding.sample_and_blend(0, 1, nullptr, 1, rest, result, result, true);
	binding.sample_and_blend(0, 1, nullptr, 1, rest, rest, result, true);
	binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, rest, true);
	ERR_PRINT_ON;
	CHECK(result.get_position(1).is_zero_approx());
	CHECK(rest.get_position(1).is_zero_approx());
	// Three and four bones occupy the same padded storage size, but differ in
	// their binding identity and cannot reuse each other's compact indices.
	rest.resize(4);
	result.resize(4);
	ERR_PRINT_OFF;
	binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, true);
	ERR_PRINT_ON;
	CHECK(result.get_position(1).is_zero_approx());
}

TEST_CASE("[CZAnimation] Direct sampling observes enabled changes and interpolation supports aliases") {
	Ref<Animation> animation;
	animation.instantiate();
	const int track = animation->add_track(Animation::TYPE_POSITION_3D);
	animation->position_track_insert_key(track, 0, Vector3(10, 0, 0));
	CZPoseTrackBinding binding;
	binding.reset(animation, 1);
	binding.add_track(Animation::TYPE_POSITION_3D, track, 0, 0);
	CZPoseSoA rest(CZPoseSoA::SOA);
	CZPoseSoA sample(CZPoseSoA::SOA);
	CZPoseSoA result(CZPoseSoA::SOA);
	rest.resize(1);
	result.copy_from(rest);
	animation->track_set_enabled(track, false);
	binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, true);
	CHECK(result.get_position(0).is_zero_approx());
	animation->track_set_enabled(track, true);
	binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, true);
	CHECK(result.get_position(0).is_equal_approx(Vector3(10, 0, 0)));
	CZPoseSoA expected(CZPoseSoA::SOA);
	expected.interpolate(rest, result, 0.25, false);
	result.interpolate(rest, result, 0.25, true);
	CHECK(result.get_transform(0).is_equal_approx(expected.get_transform(0)));
	rest.interpolate(rest, result, 0.5, true);
	CHECK(rest.get_position(0).is_equal_approx(Vector3(1.25, 0, 0)));
}

static real_t rotation_component_error(const Quaternion &p_expected, const Quaternion &p_actual) {
	real_t error = 0;
	for (int component = 0; component < 4; component++) {
		error = MAX(error, Math::abs(p_expected[component] - p_actual[component]));
	}
	return error;
}

TEST_CASE("[CZAnimation] Four-wide quaternion blending preserves rest order and masked tails") {
	const int counts[] = { 0, 1, 3, 4, 5, 17, 257 };
	const real_t amounts[] = { 0, 0.25, 0.99, 1, -0.2, 1.2, CMP_EPSILON * 0.25, 0.0001 };
	const real_t tolerance = sizeof(real_t) == sizeof(double) ? 1e-12 : 1e-6;
	real_t maximum_error = 0;
	for (int count : counts) {
		CZPoseSoA rest(CZPoseSoA::SOA), sample(CZPoseSoA::SOA), scalar(CZPoseSoA::SOA), simd(CZPoseSoA::SOA);
		rest.resize(count);
		sample.resize(count);
		for (int bone = 0; bone < count; bone++) {
			const Vector3 axis = Vector3(1 + bone % 3, 2 + bone % 5, 3 + bone % 7).normalized();
			rest.set_rotation(bone, Quaternion(axis, (bone % 11) * 0.13));
		}
		scalar.copy_from(rest);
		simd.copy_from(rest);
		LocalVector<real_t> weights;
		weights.resize(rest.padded_size());
		for (int pass = 0; pass < 8; pass++) {
			for (int bone = 0; bone < rest.padded_size(); bone++) {
				weights[bone] = bone < count ? amounts[(bone + pass) % 8] : 0;
				if (bone >= count) {
					continue;
				}
				const Vector3 axis = Vector3(3 + pass, 1 + bone % 5, 2 + bone % 7).normalized();
				Quaternion value;
				switch ((bone + pass) % 6) {
					case 0:
						value = rest.get_rotation(bone);
						break;
					case 1:
						value = -rest.get_rotation(bone);
						break;
					case 2:
						value = rest.get_rotation(bone) * Quaternion(axis, 1e-7);
						break;
					case 3:
						value = rest.get_rotation(bone) * Quaternion(axis, Math::PI - 1e-5);
						break;
					case 4:
						value = Quaternion(axis, 2.8);
						break;
					default:
						value = -Quaternion(axis, -2.8);
						break;
				}
				sample.set_rotation(bone, value);
			}
			scalar.blend(sample, rest, weights.ptr(), weights.ptr(), weights.ptr(), false);
			simd.blend(sample, rest, weights.ptr(), weights.ptr(), weights.ptr(), true);
			for (int bone = 0; bone < count; bone++) {
				const real_t error = rotation_component_error(scalar.get_rotation(bone), simd.get_rotation(bone));
				maximum_error = MAX(maximum_error, error);
				CHECK(error <= tolerance);
				CHECK(simd.get_rotation(bone).is_normalized());
			}
			for (int bone = count; bone < rest.padded_size(); bone++) {
				CHECK(simd.data(CZPoseSoA::ROTATION_X)[bone] == 0);
				CHECK(simd.data(CZPoseSoA::ROTATION_W)[bone] == 1);
			}
		}
	}
	print_line("CZ_ANIMATION_BLEND_MAX_COMPONENT_ERROR=" + String::num_scientific(maximum_error));
}

TEST_CASE("[CZAnimation] Four-wide slerp matches official hemisphere near-angle and extrapolation branches") {
	const int counts[] = { 0, 1, 3, 4, 5, 17, 257 };
	const real_t amounts[] = { 0, 1, 0.0001, 0.37, 0.9999, -0.25, 1.25 };
	const real_t tolerance = sizeof(real_t) == sizeof(double) ? 1e-12 : 1e-6;
	real_t maximum_error = 0;
	for (int count : counts) {
		CZPoseSoA from(CZPoseSoA::SOA), to(CZPoseSoA::SOA), scalar(CZPoseSoA::SOA), simd(CZPoseSoA::SOA);
		from.resize(count);
		to.resize(count);
		for (int bone = 0; bone < count; bone++) {
			const Vector3 axis = Vector3(1 + bone % 3, 2 + bone % 5, 3 + bone % 7).normalized();
			const Quaternion rotation(axis, (bone % 17) * 0.17);
			from.set_rotation(bone, rotation);
			const real_t angle = bone % 3 == 0 ? 0 : (bone % 3 == 1 ? 1e-7 : Math::PI - 1e-5);
			const Quaternion target = rotation * Quaternion(Vector3(1, 0, 0), angle);
			to.set_rotation(bone, bone % 2 == 0 ? target : -target);
		}
		for (real_t weight : amounts) {
			scalar.interpolate(from, to, weight, false);
			simd.interpolate(from, to, weight, true);
			for (int bone = 0; bone < count; bone++) {
				const Quaternion expected = from.get_rotation(bone).slerp(to.get_rotation(bone), weight);
				const real_t error = rotation_component_error(expected, simd.get_rotation(bone));
				maximum_error = MAX(maximum_error, error);
				CHECK(error <= tolerance);
				CHECK(rotation_component_error(expected, scalar.get_rotation(bone)) <= tolerance);
			}
			// Input/output aliasing remains safe after the four-wide implementation.
			simd.copy_from(from);
			simd.interpolate(simd, to, weight, true);
			for (int bone = 0; bone < count; bone++) {
				CHECK(rotation_component_error(scalar.get_rotation(bone), simd.get_rotation(bone)) <= tolerance);
			}
		}
	}
	print_line("CZ_ANIMATION_SLERP_MAX_COMPONENT_ERROR=" + String::num_scientific(maximum_error));
}

TEST_CASE("[CZAnimation] Lazy sampling leaves zero contribution scratch untouched and legacy refreshes it") {
	for (bool simd : { false, true }) {
		for (int count : { 0, 1, 3, 4, 5, 17 }) {
			Ref<Animation> animation;
			animation.instantiate();
			const int track = animation->add_track(Animation::TYPE_POSITION_3D);
			animation->position_track_insert_key(track, 0, Vector3(10, 20, 30));
			CZPoseTrackBinding fast_binding, legacy_binding;
			fast_binding.reset(animation, count);
			legacy_binding.reset(animation, count);
			if (count > 0) {
				fast_binding.add_track(Animation::TYPE_POSITION_3D, track, count - 1, 0);
				legacy_binding.add_track(Animation::TYPE_POSITION_3D, track, count - 1, 0);
			}
			CZPoseSoA rest(CZPoseSoA::SOA), fast(CZPoseSoA::SOA), legacy(CZPoseSoA::SOA), fast_sample(CZPoseSoA::SOA), legacy_sample(CZPoseSoA::SOA);
			rest.resize(count);
			for (int bone = 0; bone < count; bone++) {
				rest.set_position(bone, Vector3(bone, 2, 3));
				rest.set_scale(bone, Vector3(-1, 2, 3));
			}
			fast.copy_from(rest);
			legacy.copy_from(rest);
			fast_sample.copy_from(rest);
			legacy_sample.copy_from(rest);
			if (count > 0) {
				fast_sample.set_position(count - 1, Vector3(111, 222, 333));
				legacy_sample.set_position(count - 1, Vector3(111, 222, 333));
			}
			LocalVector<real_t> weights = { 0 };
			CZPoseTrackBinding::SamplingStatistics fast_statistics, legacy_statistics;
			fast_binding.sample_and_blend(0, 1, &weights, 1, rest, fast_sample, fast, simd, true, &fast_statistics);
			legacy_binding.sample_and_blend(0, 1, &weights, 1, rest, legacy_sample, legacy, simd, false, &legacy_statistics);
			CHECK(fast_statistics.calls == 1);
			CHECK(fast_statistics.zero_weight_calls == 1);
			CHECK(fast_statistics.no_written_calls == 1);
			CHECK(fast_statistics.empty_binding_calls == uint64_t(count == 0));
			CHECK(legacy_statistics.zero_weight_calls == 1);
			CHECK(legacy_statistics.no_written_calls == 1);
			for (int bone = 0; bone < count; bone++) {
				CHECK(fast.get_transform(bone).is_equal_approx(rest.get_transform(bone)));
				CHECK(legacy.get_transform(bone).is_equal_approx(rest.get_transform(bone)));
				CHECK(legacy_sample.get_position(bone).is_equal_approx(rest.get_position(bone)));
			}
			if (count > 0) {
				CHECK(fast_sample.get_position(count - 1) == Vector3(111, 222, 333));
			}
		}
	}
}

TEST_CASE("[CZAnimation] Lazy sampling clears stale channel weights and preserves ordered extrapolation") {
	const real_t tolerance = sizeof(real_t) == sizeof(double) ? 1e-12 : 1e-6;
	const Vector3 masks[] = { Vector3(1, 0, 0), Vector3(), Vector3(0, 0.5, 0), Vector3(0, 0, -0.25), Vector3(1.25, 1.25, 1.25), Vector3(-0.5, -0.5, -0.5), Vector3() };
	for (bool simd : { false, true }) {
		for (int count : { 1, 3, 4, 5, 17, 257 }) {
			Ref<Animation> animation;
			animation.instantiate();
			const int position = animation->add_track(Animation::TYPE_POSITION_3D);
			const int rotation = animation->add_track(Animation::TYPE_ROTATION_3D);
			const int scale = animation->add_track(Animation::TYPE_SCALE_3D);
			animation->position_track_insert_key(position, 0, Vector3(10, -3, 4));
			animation->rotation_track_insert_key(rotation, 0, -Quaternion(Vector3(0, 1, 0), Math::PI - 0.001));
			animation->scale_track_insert_key(scale, 0, Vector3(2, -3, 4));
			CZPoseTrackBinding fast_binding, legacy_binding;
			fast_binding.reset(animation, count);
			legacy_binding.reset(animation, count);
			for (int bone = 0; bone < count; bone++) {
				fast_binding.add_track(Animation::TYPE_POSITION_3D, position, bone, 0);
				fast_binding.add_track(Animation::TYPE_ROTATION_3D, rotation, bone, 1);
				fast_binding.add_track(Animation::TYPE_SCALE_3D, scale, bone, 2);
				legacy_binding.add_track(Animation::TYPE_POSITION_3D, position, bone, 0);
				legacy_binding.add_track(Animation::TYPE_ROTATION_3D, rotation, bone, 1);
				legacy_binding.add_track(Animation::TYPE_SCALE_3D, scale, bone, 2);
			}
			CZPoseSoA rest(CZPoseSoA::SOA), fast(CZPoseSoA::SOA), legacy(CZPoseSoA::SOA), fast_sample(CZPoseSoA::SOA), legacy_sample(CZPoseSoA::SOA);
			rest.resize(count);
			for (int bone = 0; bone < count; bone++) {
				rest.set_position(bone, Vector3(1, 2, 3));
				rest.set_rotation(bone, Quaternion(Vector3(1, 0, 0), 0.25));
				rest.set_scale(bone, Vector3(-1, 2, 3));
			}
			fast.copy_from(rest);
			legacy.copy_from(rest);
			CZPoseTrackBinding::SamplingStatistics statistics;
			for (const Vector3 &mask : masks) {
				LocalVector<real_t> weights = { mask.x, mask.y, mask.z };
				fast_binding.sample_and_blend(0, 1, &weights, 1.3, rest, fast_sample, fast, simd, true, &statistics);
				legacy_binding.sample_and_blend(0, 1, &weights, 1.3, rest, legacy_sample, legacy, simd, false);
				for (int component = 0; component < CZPoseSoA::COMPONENT_COUNT; component++) {
					for (int bone = 0; bone < fast.padded_size(); bone++) {
						CHECK(Math::abs(fast.data(CZPoseSoA::Component(component))[bone] - legacy.data(CZPoseSoA::Component(component))[bone]) <= tolerance);
					}
				}
			}
			CHECK(statistics.calls == 7);
			CHECK(statistics.zero_weight_calls == 2);
			CHECK(statistics.no_written_calls == 2);
			CHECK(statistics.empty_binding_calls == 0);
		}
	}
}

TEST_CASE("[CZAnimation] Lazy sampling distinguishes absent keys disabled tracks and empty bindings") {
	for (bool simd : { false, true }) {
		for (bool fastpaths : { false, true }) {
			Ref<Animation> animation;
			animation.instantiate();
			const int track = animation->add_track(Animation::TYPE_POSITION_3D);
			CZPoseTrackBinding binding;
			binding.reset(animation, 1);
			binding.add_track(Animation::TYPE_POSITION_3D, track, 0, 0);
			CZPoseSoA rest(CZPoseSoA::SOA), result(CZPoseSoA::SOA), sample(CZPoseSoA::SOA);
			rest.resize(1);
			rest.set_position(0, Vector3(1, 2, 3));
			result.copy_from(rest);
			sample.copy_from(rest);
			sample.set_position(0, Vector3(10, 20, 30));
			CZPoseTrackBinding::SamplingStatistics statistics;
			binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, simd, fastpaths, &statistics);
			CHECK(result.get_position(0) == rest.get_position(0));
			CHECK(statistics.calls == 1);
			CHECK(statistics.zero_weight_calls == 0);
			CHECK(statistics.no_written_calls == 1);
			CHECK(statistics.empty_binding_calls == 0);
			CHECK(sample.get_position(0) == (fastpaths ? Vector3(10, 20, 30) : rest.get_position(0)));
			animation->track_set_enabled(track, false);
			binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, simd, fastpaths, &statistics);
			CHECK(statistics.zero_weight_calls == 1);
			CHECK(statistics.no_written_calls == 2);
			binding.reset(animation, 1);
			binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, simd, fastpaths, &statistics);
			CHECK(statistics.calls == 3);
			CHECK(statistics.zero_weight_calls == 2);
			CHECK(statistics.no_written_calls == 3);
			CHECK(statistics.empty_binding_calls == 1);
		}
	}
}

TEST_CASE("[CZAnimation] Lazy sampling preserves near-zero negative over-one and NaN weight semantics") {
	const real_t tolerance = sizeof(real_t) == sizeof(double) ? 1e-12 : 1e-6;
	const Vector2 amounts[] = { Vector2(0, 1), Vector2(CMP_EPSILON * 0.5, 1), Vector2(-CMP_EPSILON * 0.5, 1), Vector2(-0.5, 1), Vector2(1.25, 1), Vector2(real_t(Math::NaN), 1), Vector2(0, real_t(Math::NaN)), Vector2(real_t(Math::NaN), 0) };
	Ref<Animation> animation;
	animation.instantiate();
	const int track = animation->add_track(Animation::TYPE_POSITION_3D);
	animation->position_track_insert_key(track, 0, Vector3(10, 20, 30));
	for (bool simd : { false, true }) {
		for (const Vector2 &amount : amounts) {
			CZPoseTrackBinding fast_binding, legacy_binding;
			fast_binding.reset(animation, 1);
			legacy_binding.reset(animation, 1);
			fast_binding.add_track(Animation::TYPE_POSITION_3D, track, 0, 0);
			legacy_binding.add_track(Animation::TYPE_POSITION_3D, track, 0, 0);
			CZPoseSoA rest(CZPoseSoA::SOA), fast(CZPoseSoA::SOA), legacy(CZPoseSoA::SOA), fast_sample(CZPoseSoA::SOA), legacy_sample(CZPoseSoA::SOA);
			rest.resize(1);
			rest.set_position(0, Vector3(1, 2, 3));
			fast.copy_from(rest);
			legacy.copy_from(rest);
			CZPoseTrackBinding::SamplingStatistics statistics;
			LocalVector<real_t> weights = { amount.y };
			fast_binding.sample_and_blend(0, amount.x, &weights, 1, rest, fast_sample, fast, simd, true, &statistics);
			legacy_binding.sample_and_blend(0, amount.x, &weights, 1, rest, legacy_sample, legacy, simd, false);
			for (int component = 0; component < CZPoseSoA::COMPONENT_COUNT; component++) {
				for (int bone = 0; bone < fast.padded_size(); bone++) {
					const real_t expected = legacy.data(CZPoseSoA::Component(component))[bone];
					const real_t actual = fast.data(CZPoseSoA::Component(component))[bone];
					if (Math::is_nan(expected)) {
						CHECK(Math::is_nan(actual));
					} else {
						CHECK(Math::abs(actual - expected) <= tolerance);
					}
				}
			}
			CHECK(statistics.calls == 1);
			CHECK(statistics.zero_weight_calls == uint64_t(Math::is_zero_approx(amount.x * amount.y)));
			CHECK(statistics.no_written_calls == uint64_t(Math::is_zero_approx(amount.x * amount.y)));
		}
	}
}

TEST_CASE("[CZAnimation] Certified exact zero sampling preserves accumulated poses and padded tails") {
	const real_t tolerance = sizeof(real_t) == sizeof(double) ? 1e-12 : 1e-6;
	for (int count : { 0, 1, 3, 5, 17, 257 }) {
		Ref<Animation> animation;
		animation.instantiate();
		for (int bone = 0; bone < count; bone++) {
			const int track = animation->add_track(Animation::TYPE_POSITION_3D);
			animation->position_track_insert_key(track, 0, Vector3(10 + bone, 20, 30));
		}
		for (bool simd : { false, true }) {
			for (bool fastpaths : { false, true }) {
				for (real_t scalar_weight : { real_t(0), real_t(-0.0), real_t(-0.5), real_t(2) }) {
					CZPoseTrackBinding binding, legacy_binding;
					binding.reset(animation, count);
					legacy_binding.reset(animation, count);
					LocalVector<real_t> weights;
					weights.resize(count);
					CZPoseSoA rest(CZPoseSoA::SOA), sample(CZPoseSoA::SOA), legacy_sample(CZPoseSoA::SOA), result(CZPoseSoA::SOA), legacy(CZPoseSoA::SOA);
					rest.resize(count);
					for (int bone = 0; bone < count; bone++) {
						binding.add_track(Animation::TYPE_POSITION_3D, bone, bone, bone);
						legacy_binding.add_track(Animation::TYPE_POSITION_3D, bone, bone, bone);
						weights[bone] = bone % 2 ? real_t(-0.0) : real_t(0);
						rest.set_position(bone, Vector3(bone, 2, 3));
						rest.set_scale(bone, Vector3(-1, 2, 3));
					}
					result.copy_from(rest);
					for (int bone = 0; bone < count; bone++) {
						result.set_position(bone, Vector3(31 + bone, 41, 59));
					}
					legacy.copy_from(result);
					sample.copy_from(result);
					CZPoseTrackBinding::SamplingStatistics statistics;
					binding.sample_and_blend(0, scalar_weight, &weights, 1, rest, sample, result, simd, fastpaths, &statistics, true);
					legacy_binding.sample_and_blend(0, scalar_weight, &weights, 1, rest, legacy_sample, legacy, simd, false);
					for (int component = 0; component < CZPoseSoA::COMPONENT_COUNT; component++) {
						for (int bone = 0; bone < result.padded_size(); bone++) {
							CHECK(Math::abs(result.data(CZPoseSoA::Component(component))[bone] - legacy.data(CZPoseSoA::Component(component))[bone]) <= tolerance);
						}
					}
					CHECK(statistics.calls == 1);
					CHECK(statistics.zero_weight_calls == 1);
					CHECK(statistics.no_written_calls == 1);
					CHECK(statistics.empty_binding_calls == uint64_t(count == 0));
					CHECK(statistics.certified_zero_weight_calls == uint64_t(fastpaths));
				}
			}
		}
	}
}

TEST_CASE("[CZAnimation] Zero certificates retain nonfinite scalar and invalid index fallback semantics") {
	Ref<Animation> animation;
	animation.instantiate();
	const int track = animation->add_track(Animation::TYPE_POSITION_3D);
	animation->position_track_insert_key(track, 0, Vector3(10, 20, 30));
	for (bool simd : { false, true }) {
		for (int blend_index : { -1, 0, 2 }) {
			for (real_t scalar_weight : { real_t(1), real_t(Math::NaN), real_t(Math::INF), real_t(-Math::INF) }) {
				CZPoseTrackBinding binding, legacy_binding;
				binding.reset(animation, 1);
				legacy_binding.reset(animation, 1);
				binding.add_track(Animation::TYPE_POSITION_3D, track, 0, blend_index);
				legacy_binding.add_track(Animation::TYPE_POSITION_3D, track, 0, blend_index);
				CZPoseSoA rest(CZPoseSoA::SOA), sample(CZPoseSoA::SOA), result(CZPoseSoA::SOA), legacy_sample(CZPoseSoA::SOA), legacy(CZPoseSoA::SOA);
				rest.resize(1);
				result.copy_from(rest);
				legacy.copy_from(rest);
				LocalVector<real_t> weights = { real_t(0) };
				CZPoseTrackBinding::SamplingStatistics statistics;
				binding.sample_and_blend(0, scalar_weight, &weights, 1, rest, sample, result, simd, true, &statistics, true);
				legacy_binding.sample_and_blend(0, scalar_weight, &weights, 1, rest, legacy_sample, legacy, simd, false);
				for (int axis = 0; axis < 3; axis++) {
					const real_t expected = legacy.get_position(0)[axis];
					const real_t actual = result.get_position(0)[axis];
					if (Math::is_nan(expected)) {
						CHECK(Math::is_nan(actual));
					} else {
						CHECK(actual == expected);
					}
				}
				CHECK(statistics.certified_zero_weight_calls == uint64_t(blend_index == 0 && Math::is_finite(scalar_weight)));
			}
		}
	}
}

static void check_pose_layout_values(const CZPoseSoA &p_expected, const CZPoseSoA &p_actual) {
	REQUIRE(p_actual.size() == p_expected.size());
	const real_t tolerance = sizeof(real_t) == sizeof(double) ? 1e-12 : 1e-6;
	for (int bone = 0; bone < p_expected.size(); bone++) {
		const Vector3 expected[] = { p_expected.get_position(bone), p_expected.get_scale(bone) };
		const Vector3 actual[] = { p_actual.get_position(bone), p_actual.get_scale(bone) };
		for (int channel = 0; channel < 2; channel++) {
			for (int axis = 0; axis < 3; axis++) {
				if (Math::is_nan(expected[channel][axis])) {
					CHECK(Math::is_nan(actual[channel][axis]));
				} else {
					CHECK(Math::abs(actual[channel][axis] - expected[channel][axis]) <= tolerance);
				}
			}
		}
		CHECK(rotation_component_error(p_expected.get_rotation(bone), p_actual.get_rotation(bone)) <= tolerance);
	}
}

TEST_CASE("[CZAnimation] AoS scalar matches SoA rest blending and aliased interpolation") {
	for (int count : { 0, 1, 3, 4, 5, 17, 257 }) {
		CZPoseSoA soa_rest(CZPoseSoA::SOA), soa_sample(CZPoseSoA::SOA), soa_result(CZPoseSoA::SOA);
		CZPoseSoA aos_rest(CZPoseSoA::AOS), aos_sample(CZPoseSoA::AOS), aos_result(CZPoseSoA::AOS);
		for (CZPoseSoA *pose : { &soa_rest, &soa_sample, &aos_rest, &aos_sample }) {
			pose->resize(count);
		}
		for (int bone = 0; bone < count; bone++) {
			const Quaternion rest_rotation(Vector3(0, 1, 0), (bone % 17) * 0.13);
			const real_t angle = bone % 3 == 0 ? 0 : (bone % 3 == 1 ? 1e-7 : Math::PI - 1e-5);
			const Quaternion sample_rotation = rest_rotation * Quaternion(Vector3(1, 0, 0), angle);
			for (CZPoseSoA *pose : { &soa_rest, &aos_rest }) {
				pose->set_position(bone, Vector3(bone, -2, 3));
				pose->set_rotation(bone, rest_rotation);
				pose->set_scale(bone, Vector3(-1, 2, 3));
			}
			for (CZPoseSoA *pose : { &soa_sample, &aos_sample }) {
				pose->set_position(bone, Vector3(-bone, 4, -5));
				pose->set_rotation(bone, bone % 2 ? sample_rotation : -sample_rotation);
				pose->set_scale(bone, Vector3(2, -3, 4));
			}
		}
		soa_result.copy_from(soa_rest);
		aos_result.copy_from(aos_rest);
		LocalVector<real_t> weights;
		weights.resize(soa_rest.padded_size());
		for (real_t weight : { real_t(0), real_t(1), real_t(-0.25), real_t(1.25), real_t(0.37) }) {
			for (int bone = 0; bone < soa_rest.padded_size(); bone++) {
				weights[bone] = bone < count ? weight : 0;
			}
			soa_result.blend(soa_sample, soa_rest, weights.ptr(), weights.ptr(), weights.ptr(), false);
			aos_result.blend(aos_sample, aos_rest, weights.ptr(), weights.ptr(), weights.ptr(), false);
			check_pose_layout_values(soa_result, aos_result);
			soa_result.interpolate(soa_rest, soa_sample, weight, false);
			aos_result.interpolate(aos_rest, aos_sample, weight, false);
			check_pose_layout_values(soa_result, aos_result);
			for (CZPoseSoA::Layout layout : { CZPoseSoA::SOA, CZPoseSoA::AOS }) {
				const CZPoseSoA &from = layout == CZPoseSoA::SOA ? soa_rest : aos_rest;
				const CZPoseSoA &to = layout == CZPoseSoA::SOA ? soa_sample : aos_sample;
				CZPoseSoA alias(layout);
				alias.copy_from(from);
				alias.interpolate(alias, to, weight, false);
				check_pose_layout_values(soa_result, alias);
				alias.copy_from(to);
				alias.interpolate(from, alias, weight, false);
				check_pose_layout_values(soa_result, alias);
			}
		}
	}
	CZPoseSoA soa(CZPoseSoA::SOA), aos(CZPoseSoA::AOS);
	soa.resize(1);
	aos.resize(1);
	aos.set_position(0, Vector3(3, 4, 5));
	const real_t weights[4] = { 1, 0, 0, 0 };
	ERR_PRINT_OFF;
	aos.copy_from(soa);
	aos.blend(soa, aos, weights, weights, weights, false);
	aos.interpolate(aos, soa, 0.5, false);
	const real_t *data = aos.data(CZPoseSoA::POSITION_X);
	ERR_PRINT_ON;
	CHECK(data == nullptr);
	CHECK(aos.get_position(0) == Vector3(3, 4, 5));
}

TEST_CASE("[CZAnimation] AoS and SoA directly sample partial channels motion scale and zero times NaN") {
	Ref<Animation> animation;
	animation.instantiate();
	const int position = animation->add_track(Animation::TYPE_POSITION_3D);
	const int rotation = animation->add_track(Animation::TYPE_ROTATION_3D);
	animation->position_track_insert_key(position, 0, Vector3(0, 2, 1));
	animation->position_track_insert_key(position, 1, Vector3(0, 4, 3));
	animation->rotation_track_insert_key(rotation, 0, -Quaternion(Vector3(0, 1, 0), Math::PI - 1e-5));
	CZPoseSoA soa(CZPoseSoA::SOA), aos(CZPoseSoA::AOS);
	for (CZPoseSoA *result : { &soa, &aos }) {
		CZPoseSoA rest(result->get_layout()), sample(result->get_layout());
		rest.resize(3);
		rest.set_position(1, Vector3(1, 2, 3));
		rest.set_rotation(1, Quaternion(Vector3(0, 0, 1), 0.2));
		rest.set_scale(1, Vector3(-1, 2, -3));
		result->copy_from(rest);
		CZPoseTrackBinding binding;
		binding.reset(animation, 3);
		binding.add_track(Animation::TYPE_POSITION_3D, position, 1, 0);
		binding.add_track(Animation::TYPE_ROTATION_3D, rotation, 2, 1);
		LocalVector<real_t> weights = { 0.5, 0.25 };
		binding.sample_and_blend(0.5, 1, &weights, 2, rest, sample, *result, false);
		CHECK(result->get_position(1).is_equal_approx(Vector3(0.5, 4, 3.5)));
		CHECK(result->get_scale(1) == rest.get_scale(1));
		CHECK(result->get_rotation(1) == rest.get_rotation(1));
		CHECK(result->get_transform(0).is_equal_approx(rest.get_transform(0)));
		binding.reset(animation, 3);
		binding.add_track(Animation::TYPE_POSITION_3D, position, 1, 0);
		weights[0] = 0;
		binding.sample_and_blend(0.5, real_t(Math::NaN), &weights, 2, rest, sample, *result, false);
		for (int axis = 0; axis < 3; axis++) {
			CHECK(Math::is_nan(result->get_position(1)[axis]));
		}
		CHECK(result->get_scale(1) == rest.get_scale(1));
		CHECK(result->get_rotation(1) == rest.get_rotation(1));
	}
	check_pose_layout_values(soa, aos);
}

TEST_CASE("[CZAnimation] Shared track descriptions retain independent time weights and copy on modification") {
	Ref<Animation> animation;
	animation.instantiate();
	const int position = animation->add_track(Animation::TYPE_POSITION_3D);
	animation->position_track_insert_key(position, 0, Vector3());
	animation->position_track_insert_key(position, 1, Vector3(10, 0, 0));
	CZPoseTrackBinding first;
	first.reset(animation, 2);
	first.add_track(Animation::TYPE_POSITION_3D, position, 0, 0);
	first.finalize();
	CZPoseTrackBinding second = first;
	CHECK(first.shares_track_data_with(second));
	CZPoseSoA rest(CZPoseSoA::SOA), first_sample(CZPoseSoA::SOA), second_sample(CZPoseSoA::SOA), a(CZPoseSoA::SOA), b(CZPoseSoA::SOA);
	rest.resize(2);
	a.copy_from(rest);
	b.copy_from(rest);
	LocalVector<real_t> first_weights = { 0.25, 0 };
	LocalVector<real_t> second_weights = { 0.5, 1 };
	first.sample_and_blend(0.75, 1, &first_weights, 1, rest, first_sample, a, false);
	second.sample_and_blend(0.25, 1, &second_weights, 1, rest, second_sample, b, false);
	CHECK(a.get_position(0).is_equal_approx(Vector3(1.875, 0, 0)));
	CHECK(b.get_position(0).is_equal_approx(Vector3(1.25, 0, 0)));
	second.add_track(Animation::TYPE_POSITION_3D, position, 1, 1);
	second.finalize();
	CHECK_FALSE(first.shares_track_data_with(second));
	a.copy_from(rest);
	b.copy_from(rest);
	first.sample_and_blend(0.5, 1, &first_weights, 1, rest, first_sample, a, false);
	second.sample_and_blend(0.5, 1, &second_weights, 1, rest, second_sample, b, false);
	CHECK(a.get_position(1).is_zero_approx());
	CHECK(b.get_position(1).is_equal_approx(Vector3(5, 0, 0)));
	second.reset(animation, 2);
	second.finalize();
	a.copy_from(rest);
	b.copy_from(rest);
	first.sample_and_blend(0.5, 1, &first_weights, 1, rest, first_sample, a, false);
	second.sample_and_blend(0.5, 1, &second_weights, 1, rest, second_sample, b, false);
	CHECK(a.get_position(0).is_equal_approx(Vector3(1.25, 0, 0)));
	CHECK(b.get_position(0).is_zero_approx());
}

TEST_CASE("[CZAnimation] Track description interning compares all blend indices and releases unused data") {
	CZPoseTrackBinding::collect_unused_track_data();
	CZPoseTrackBinding::clear_track_data_cache();
	const Dictionary before = CZPoseTrackBinding::get_track_data_statistics();
	{
		Ref<Animation> animations[2];
		for (int i = 0; i < 2; i++) {
			animations[i].instantiate();
			const int position = animations[i]->add_track(Animation::TYPE_POSITION_3D);
			const int scale = animations[i]->add_track(Animation::TYPE_SCALE_3D);
			animations[i]->position_track_insert_key(position, 0, Vector3(10 * (i + 1), 0, 0));
			animations[i]->scale_track_insert_key(scale, 0, Vector3(2, 3, 4));
		}
		CZPoseTrackBinding first, other_animation, other_index;
		first.reset(animations[0], 1);
		other_animation.reset(animations[1], 1);
		other_index.reset(animations[0], 1);
		for (CZPoseTrackBinding *binding : { &first, &other_animation, &other_index }) {
			binding->add_track(Animation::TYPE_POSITION_3D, 0, 0, binding == &other_index ? 1 : 0);
			binding->add_track(Animation::TYPE_SCALE_3D, 1, 0, 1);
			binding->finalize();
		}
		CHECK(first.shares_track_data_with(other_animation) == CZPoseTrackBinding::is_shared_track_data_enabled());
		CHECK_FALSE(first.shares_track_data_with(other_index));
		LocalVector<real_t> weights = { 0.25, 0.75 };
		for (CZPoseTrackBinding *binding : { &first, &other_animation, &other_index }) {
			CZPoseSoA rest(CZPoseSoA::SOA), sample(CZPoseSoA::SOA), result(CZPoseSoA::SOA);
			rest.resize(1);
			result.copy_from(rest);
			binding->sample_and_blend(0, 1, &weights, 1, rest, sample, result, false);
			const real_t expected = binding == &first ? 2.5 : (binding == &other_animation ? 5 : 7.5);
			CHECK(result.get_position(0).is_equal_approx(Vector3(expected, 0, 0)));
		}
		const Dictionary live = CZPoseTrackBinding::get_track_data_statistics();
		CHECK(int64_t(live["track_descriptor_live_bytes"]) > int64_t(before["track_descriptor_live_bytes"]));
		CHECK(int64_t(live["track_descriptor_builds"]) >= int64_t(before["track_descriptor_builds"]) + 3);
		if (CZPoseTrackBinding::is_shared_track_data_enabled()) {
			CHECK(int64_t(live["track_descriptor_cache_hits"]) > int64_t(before["track_descriptor_cache_hits"]));
		} else {
			CHECK(int64_t(live["track_descriptor_cache_hits"]) == int64_t(before["track_descriptor_cache_hits"]));
		}
	}
	CZPoseTrackBinding::collect_unused_track_data();
	const Dictionary after = CZPoseTrackBinding::get_track_data_statistics();
	CHECK(int64_t(after["track_descriptor_live_count"]) == int64_t(before["track_descriptor_live_count"]));
	CHECK(int64_t(after["track_descriptor_live_bytes"]) == int64_t(before["track_descriptor_live_bytes"]));
}

TEST_CASE("[CZAnimation] Pose sampling scratch matches binding weights across interleaved clips and owners") {
	Ref<Animation> position_clip, other_clip;
	position_clip.instantiate();
	other_clip.instantiate();
	const int position = position_clip->add_track(Animation::TYPE_POSITION_3D);
	const int scale = other_clip->add_track(Animation::TYPE_SCALE_3D);
	const int rotation = other_clip->add_track(Animation::TYPE_ROTATION_3D);
	position_clip->position_track_insert_key(position, 0, Vector3(10, -3, 4));
	other_clip->scale_track_insert_key(scale, 0, Vector3(2, -3, 4));
	other_clip->rotation_track_insert_key(rotation, 0, -Quaternion(Vector3(0, 1, 0), 0.7));
	for (CZPoseSoA::Layout layout : { CZPoseSoA::SOA, CZPoseSoA::AOS }) {
		for (bool fastpaths : { false, true }) {
			for (int count : { 0, 1, 3, 4, 5, 17 }) {
				CZPoseTrackBinding positions, others;
				positions.reset(position_clip, count);
				others.reset(other_clip, count);
				if (count > 0) {
					positions.add_track(Animation::TYPE_POSITION_3D, position, count - 1, 0);
					others.add_track(Animation::TYPE_SCALE_3D, scale, count - 1, 0);
					others.add_track(Animation::TYPE_ROTATION_3D, rotation, count - 1, 1);
				}
				CZPoseSamplingScratch scratch, other_owner;
				scratch.resize(count);
				other_owner.resize(count);
				CZPoseSoA rest(layout), reference(layout), reused(layout), sample(layout), shared_sample(layout), unrelated(layout);
				rest.resize(count);
				for (int bone = 0; bone < count; bone++) {
					rest.set_position(bone, Vector3(bone, 2, 3));
					rest.set_scale(bone, Vector3(-1, 2, 3));
				}
				reference.copy_from(rest);
				reused.copy_from(rest);
				for (int step = 0; step < 5; step++) {
					CZPoseTrackBinding &binding = step == 1 || step == 4 ? others : positions;
					LocalVector<real_t> weights = { step == 2 || step == 3 ? real_t(0) : real_t(0.5), real_t(0.25) };
					const real_t amount = step == 3 ? real_t(Math::NaN) : real_t(1);
					binding.sample_and_blend(0, amount, &weights, 1.3, rest, sample, reference, true, fastpaths);
					const uint64_t growths = CZPoseSamplingScratch::get_buffer_growths_total();
					binding.sample_and_blend(0, amount, &weights, 1.3, rest, shared_sample, reused, true, fastpaths, nullptr, false, &scratch);
					CHECK(CZPoseSamplingScratch::get_buffer_growths_total() == growths);
					check_pose_layout_values(reference, reused);
					if (count > 0) {
						const real_t saved = scratch.position_weights[count - 1];
						LocalVector<real_t> different_weights = { 1.25 };
						unrelated.copy_from(rest);
						positions.sample_and_blend(0, 1, &different_weights, 1, rest, sample, unrelated, false, true, nullptr, false, &other_owner);
						if (Math::is_nan(saved)) {
							CHECK(Math::is_nan(scratch.position_weights[count - 1]));
						} else {
							CHECK(scratch.position_weights[count - 1] == saved);
						}
					}
				}
				for (uint32_t bone = count; bone < scratch.position_weights.size(); bone++) {
					CHECK(scratch.position_weights[bone] == 0);
					CHECK(scratch.rotation_weights[bone] == 0);
					CHECK(scratch.scale_weights[bone] == 0);
				}
			}
		}
	}
}

TEST_CASE("[CZAnimation] Sampling scratch keeps capacity across binding bone count changes") {
	Ref<Animation> animation;
	animation.instantiate();
	const int track = animation->add_track(Animation::TYPE_POSITION_3D);
	animation->position_track_insert_key(track, 0, Vector3(4, 5, 6));
	CZPoseTrackBinding binding;
	CZPoseSamplingScratch scratch;
	uint64_t larger_bytes = 0;
	for (int count : { 1, 5, 3, 17, 0 }) {
		binding.reset(animation, count);
		scratch.resize(count);
		if (count > 0) {
			binding.add_track(Animation::TYPE_POSITION_3D, track, count - 1, 0);
		}
		if (count == 5) {
			larger_bytes = scratch.get_storage_bytes();
		} else if (count == 3) {
			CHECK(scratch.get_storage_bytes() == larger_bytes);
		}
		if (CZPoseTrackBinding::is_sample_scratch_reuse_enabled()) {
			CHECK(binding.get_sample_scratch_storage_bytes() == 0);
		} else {
			CHECK(binding.get_sample_scratch_storage_bytes() >= uint64_t((count + 3) & ~3) * 3 * sizeof(real_t));
		}
		CZPoseSoA rest(CZPoseSoA::SOA), sample(CZPoseSoA::SOA), result(CZPoseSoA::SOA);
		rest.resize(count);
		result.copy_from(rest);
		const uint64_t growths = CZPoseSamplingScratch::get_buffer_growths_total();
		binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, false, true, nullptr, false, &scratch);
		CHECK(CZPoseSamplingScratch::get_buffer_growths_total() == growths);
		if (count > 0) {
			CHECK(result.get_position(count - 1) == Vector3(4, 5, 6));
		}
	}
}

TEST_CASE("[CZAnimation] First blend in place preserves rest math quaternion masks and ordered accumulation") {
	const real_t amounts[] = { 0, CMP_EPSILON * 0.25, -CMP_EPSILON * 0.25, -0.25, 1, 1.25, 0.37 };
	for (CZPoseSoA::Layout layout : { CZPoseSoA::SOA, CZPoseSoA::AOS }) {
		for (bool simd : { false, true }) {
			for (int count : { 0, 1, 3, 4, 5, 17, 257 }) {
				CZPoseSoA rest(layout), sample(layout), reference(layout), in_place(layout);
				rest.resize(count);
				sample.resize(count);
				for (int bone = 0; bone < count; bone++) {
					const Quaternion rotation(Vector3(0, 1, 0), (bone % 17) * 0.13);
					const real_t angle = bone % 3 == 0 ? 0 : (bone % 3 == 1 ? 1e-7 : Math::PI - 1e-5);
					const Quaternion target = rotation * Quaternion(Vector3(1, 0, 0), angle);
					rest.set_position(bone, Vector3(bone, -2, 3));
					rest.set_rotation(bone, rotation);
					rest.set_scale(bone, Vector3(-1, 2, 3));
					sample.set_position(bone, Vector3(-bone, 4, -5));
					sample.set_rotation(bone, bone % 2 ? target : -target);
					sample.set_scale(bone, Vector3(2, -3, 4));
				}
				LocalVector<real_t> positions, rotations, scales;
				positions.resize(rest.padded_size());
				rotations.resize(rest.padded_size());
				scales.resize(rest.padded_size());
				for (int pass = 0; pass < 8; pass++) {
					for (int bone = 0; bone < rest.padded_size(); bone++) {
						positions[bone] = pass && bone < count ? amounts[(bone + pass) % 7] : 0;
						rotations[bone] = pass && bone < count ? amounts[(bone + pass + 1) % 7] : 0;
						scales[bone] = pass && bone < count ? amounts[(bone + pass + 2) % 7] : 0;
					}
					reference.copy_from(rest);
					in_place.copy_from(sample);
					reference.blend(sample, rest, positions.ptr(), rotations.ptr(), scales.ptr(), simd);
					in_place.blend(in_place, rest, positions.ptr(), rotations.ptr(), scales.ptr(), simd, true);
					check_pose_layout_values(reference, in_place);
					reference.blend(sample, rest, rotations.ptr(), scales.ptr(), positions.ptr(), simd);
					in_place.blend(sample, rest, rotations.ptr(), scales.ptr(), positions.ptr(), simd);
					check_pose_layout_values(reference, in_place);
					if (layout == CZPoseSoA::SOA) {
						for (int bone = count; bone < rest.padded_size(); bone++) {
							CHECK(in_place.data(CZPoseSoA::POSITION_X)[bone] == 0);
							CHECK(in_place.data(CZPoseSoA::ROTATION_W)[bone] == 1);
							CHECK(in_place.data(CZPoseSoA::SCALE_X)[bone] == 1);
						}
					}
				}
			}
		}
	}
}

TEST_CASE("[CZAnimation] First sampled contribution saves one pose copy without dropping zero or NaN math") {
	Ref<Animation> position_clip, rotation_clip;
	position_clip.instantiate();
	rotation_clip.instantiate();
	const int position = position_clip->add_track(Animation::TYPE_POSITION_3D);
	const int rotation = rotation_clip->add_track(Animation::TYPE_ROTATION_3D);
	position_clip->position_track_insert_key(position, 0, Vector3(10, -3, 4));
	rotation_clip->rotation_track_insert_key(rotation, 0, -Quaternion(Vector3(0, 1, 0), Math::PI - 1e-5));
	const Vector2 cases[] = { Vector2(), Vector2(0, 1), Vector2(CMP_EPSILON * 0.25, 1), Vector2(0.5, 0.25), Vector2(-0.25, 1.25), Vector2(real_t(Math::NaN), 0) };
	for (CZPoseSoA::Layout layout : { CZPoseSoA::SOA, CZPoseSoA::AOS }) {
		for (bool simd : { false, true }) {
			for (bool fastpaths : { false, true }) {
				for (int count : { 1, 5 }) {
					CZPoseTrackBinding positions, rotations;
					positions.reset(position_clip, count);
					rotations.reset(rotation_clip, count);
					positions.add_track(Animation::TYPE_POSITION_3D, position, count - 1, 0);
					rotations.add_track(Animation::TYPE_ROTATION_3D, rotation, count - 1, 0);
					CZPoseSoA rest(layout);
					rest.resize(count);
					for (int bone = 0; bone < count; bone++) {
						rest.set_position(bone, Vector3(bone, 2, 3));
						rest.set_rotation(bone, Quaternion(Vector3(1, 0, 0), 0.3));
						rest.set_scale(bone, Vector3(-1, 2, 3));
					}
					for (const Vector2 &amount : cases) {
						CZPoseSoA reference(layout), optimized(layout), reference_sample(layout), optimized_sample(layout);
						reference.copy_from(rest);
						const uint64_t initial_copy = reference.get_copy_bytes();
						optimized.resize(count);
						bool contributed = false;
						for (int clip = 0; clip < 2; clip++) {
							CZPoseTrackBinding &binding = clip == 0 ? positions : rotations;
							LocalVector<real_t> weights = { clip == 0 && Math::is_nan(amount.x) ? real_t(0) : real_t(1) };
							const real_t weight = clip == 0 ? amount.x : amount.y;
							const bool expected = binding.sample_and_blend(0, weight, &weights, 1.3, rest, reference_sample, reference, simd, fastpaths);
							const bool actual = binding.sample_and_blend(0, weight, &weights, 1.3, rest, optimized_sample, optimized, simd, fastpaths, nullptr, false, nullptr, !contributed);
							CHECK(actual == expected);
							contributed |= actual;
						}
						if (!contributed) {
							optimized.copy_from(rest);
						}
						check_pose_layout_values(reference, optimized);
						const uint64_t reference_copies = reference.get_copy_bytes() + reference_sample.get_copy_bytes();
						const uint64_t optimized_copies = optimized.get_copy_bytes() + optimized_sample.get_copy_bytes();
						CHECK(optimized_copies + (contributed ? initial_copy : 0) == reference_copies);
					}
				}
			}
		}
	}
}

TEST_CASE("[CZAnimation] Full track bindings sample only active prefixes across padded tails") {
	for (CZPoseSoA::Layout layout : { CZPoseSoA::SOA, CZPoseSoA::AOS }) {
		for (int count : { 1, 5, 17 }) {
			Ref<Animation> animation;
			animation.instantiate();
			CZPoseTrackBinding complete;
			complete.reset(animation, count);
			for (int bone = 0; bone < count; bone++) {
				for (int channel = 0; channel < 3; channel++) {
					const Animation::TrackType type = Animation::TrackType(Animation::TYPE_POSITION_3D + channel);
					const int track = animation->add_track(type);
					if (channel == 0) {
						animation->position_track_insert_key(track, 0, Vector3(bone + 1, 2, 3));
					} else if (channel == 1) {
						animation->rotation_track_insert_key(track, 0, Quaternion(Vector3(0, 1, 0), 0.2 * (bone + 1)));
					} else {
						animation->scale_track_insert_key(track, 0, Vector3(1.2, 0.8, -0.5));
					}
					complete.add_track(type, track, bone, track);
				}
			}
			complete.finalize();
			CZPoseSamplingScratch scratch;
			for (int active : { 0, 1, MIN(count, 3), count, 1, count }) {
				CZPoseTrackBinding prefix;
				prefix.reset(animation, active);
				for (int bone = 0; bone < active; bone++) {
					for (int channel = 0; channel < 3; channel++) {
						prefix.add_track(Animation::TrackType(Animation::TYPE_POSITION_3D + channel), bone * 3 + channel, bone, bone * 3 + channel);
					}
				}
				prefix.finalize();
				for (bool fastpaths : { false, true }) {
					for (bool simd : { false, true }) {
						CZPoseSoA rest(layout), expected(layout), actual(layout), expected_sample(layout), actual_sample(layout);
						rest.resize(active);
						expected.copy_from(rest);
						actual.copy_from(rest);
						CHECK(complete.sample_and_blend(0, 0.5, nullptr, 1.25, rest, actual_sample, actual, simd, fastpaths, nullptr, false, &scratch) ==
								prefix.sample_and_blend(0, 0.5, nullptr, 1.25, rest, expected_sample, expected, simd, fastpaths));
						check_pose_layout_values(expected, actual);
						CHECK(actual.size() == active);
					}
				}
			}
		}
	}
}

} // namespace TestCZAnimation
