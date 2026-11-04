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

#include "tests/test_macros.h"

#include "modules/cz_animation/bone_lod.h"
#include "modules/cz_animation/pose_soa.h"

namespace TestCZAnimation {

TEST_CASE("[CZAnimation] SoA scalar and SIMD preserve slerp and padded tails") {
	const int counts[] = { 0, 1, 3, 4, 5, 17 };
	for (int count : counts) {
		CZPoseSoA from;
		CZPoseSoA to;
		CZPoseSoA scalar;
		CZPoseSoA simd;
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
	CZPoseSoA rest;
	CZPoseSoA sample;
	CZPoseSoA scalar;
	CZPoseSoA simd;
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
	CZPoseSoA full;
	full.resize(parents.size());
	for (int i = 0; i < full.size(); i++) {
		full.set_position(i, rest[i].origin);
	}
	full.set_position(3, Vector3(5, 0, 0));
	CZPoseSoA compact;
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
	CZPoseSoA rest;
	rest.resize(3);
	rest.set_position(1, Vector3(0, 1, 0));
	rest.set_scale(1, Vector3(1, 2, -1));
	CZPoseSoA result;
	result.copy_from(rest);
	CZPoseSoA sample;
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
	CZPoseSoA rest;
	CZPoseSoA sample;
	CZPoseSoA result;
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
	CZPoseSoA rest;
	CZPoseSoA sample;
	CZPoseSoA result;
	rest.resize(1);
	result.copy_from(rest);
	animation->track_set_enabled(track, false);
	binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, true);
	CHECK(result.get_position(0).is_zero_approx());
	animation->track_set_enabled(track, true);
	binding.sample_and_blend(0, 1, nullptr, 1, rest, sample, result, true);
	CHECK(result.get_position(0).is_equal_approx(Vector3(10, 0, 0)));
	CZPoseSoA expected;
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
		CZPoseSoA rest, sample, scalar, simd;
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
		CZPoseSoA from, to, scalar, simd;
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

} // namespace TestCZAnimation
