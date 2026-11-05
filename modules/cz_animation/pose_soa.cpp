/**************************************************************************/
/*  pose_soa.cpp                                                          */
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

#include "pose_soa.h"

#include "core/os/os.h"
#include "core/os/thread.h"

#if !defined(REAL_T_IS_DOUBLE) && (defined(__SSE2__) || defined(_M_X64))
#define CZ_POSE_SSE2
#include <emmintrin.h>
#elif !defined(REAL_T_IS_DOUBLE) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#define CZ_POSE_NEON
#include <arm_neon.h>
#endif

#include <cstring>

namespace {

#if defined(CZ_POSE_SSE2) || defined(CZ_POSE_NEON)
#ifdef CZ_POSE_SSE2
using PoseWide = __m128;
_FORCE_INLINE_ PoseWide wide_load(const float *p_values) {
	return _mm_loadu_ps(p_values);
}
_FORCE_INLINE_ void wide_store(float *r_values, PoseWide p_value) {
	_mm_storeu_ps(r_values, p_value);
}
_FORCE_INLINE_ PoseWide wide_splat(float p_value) {
	return _mm_set1_ps(p_value);
}
_FORCE_INLINE_ PoseWide wide_add(PoseWide p_a, PoseWide p_b) {
	return _mm_add_ps(p_a, p_b);
}
_FORCE_INLINE_ PoseWide wide_sub(PoseWide p_a, PoseWide p_b) {
	return _mm_sub_ps(p_a, p_b);
}
_FORCE_INLINE_ PoseWide wide_mul(PoseWide p_a, PoseWide p_b) {
	return _mm_mul_ps(p_a, p_b);
}
_FORCE_INLINE_ PoseWide wide_div(PoseWide p_a, PoseWide p_b) {
	return _mm_div_ps(p_a, p_b);
}
_FORCE_INLINE_ PoseWide wide_sqrt(PoseWide p_value) {
	return _mm_sqrt_ps(p_value);
}
_FORCE_INLINE_ PoseWide wide_negate(PoseWide p_value) {
	return _mm_xor_ps(p_value, _mm_set1_ps(-0.0f));
}
_FORCE_INLINE_ PoseWide wide_select(PoseWide p_previous, PoseWide p_result, const bool *p_active) {
	const __m128 mask = _mm_castsi128_ps(_mm_set_epi32(p_active[3] ? -1 : 0, p_active[2] ? -1 : 0, p_active[1] ? -1 : 0, p_active[0] ? -1 : 0));
	return _mm_or_ps(_mm_and_ps(mask, p_result), _mm_andnot_ps(mask, p_previous));
}
#else
using PoseWide = float32x4_t;
_FORCE_INLINE_ PoseWide wide_load(const float *p_values) {
	return vld1q_f32(p_values);
}
_FORCE_INLINE_ void wide_store(float *r_values, PoseWide p_value) {
	vst1q_f32(r_values, p_value);
}
_FORCE_INLINE_ PoseWide wide_splat(float p_value) {
	return vdupq_n_f32(p_value);
}
_FORCE_INLINE_ PoseWide wide_add(PoseWide p_a, PoseWide p_b) {
	return vaddq_f32(p_a, p_b);
}
_FORCE_INLINE_ PoseWide wide_sub(PoseWide p_a, PoseWide p_b) {
	return vsubq_f32(p_a, p_b);
}
_FORCE_INLINE_ PoseWide wide_mul(PoseWide p_a, PoseWide p_b) {
	return vmulq_f32(p_a, p_b);
}
_FORCE_INLINE_ PoseWide wide_negate(PoseWide p_value) {
	return vnegq_f32(p_value);
}
_FORCE_INLINE_ PoseWide wide_select(PoseWide p_previous, PoseWide p_result, const bool *p_active) {
	const uint32_t mask[4] = { p_active[0] ? UINT32_MAX : 0, p_active[1] ? UINT32_MAX : 0, p_active[2] ? UINT32_MAX : 0, p_active[3] ? UINT32_MAX : 0 };
	return vbslq_f32(vld1q_u32(mask), p_result, p_previous);
}
_FORCE_INLINE_ PoseWide wide_div(PoseWide p_a, PoseWide p_b) {
#if defined(__aarch64__) || defined(_M_ARM64)
	return vdivq_f32(p_a, p_b);
#else
	// ARM32 has no exact four-wide division. Avoid reciprocal estimates.
	float a[4], b[4];
	wide_store(a, p_a);
	wide_store(b, p_b);
	for (int lane = 0; lane < 4; lane++) {
		a[lane] /= b[lane];
	}
	return wide_load(a);
#endif
}
_FORCE_INLINE_ PoseWide wide_sqrt(PoseWide p_value) {
#if defined(__aarch64__) || defined(_M_ARM64)
	return vsqrtq_f32(p_value);
#else
	float values[4];
	wide_store(values, p_value);
	for (float &value : values) {
		value = Math::sqrt(value);
	}
	return wide_load(values);
#endif
}
#endif

struct WideQuaternion {
	PoseWide x, y, z, w;
};

_FORCE_INLINE_ WideQuaternion load_rotation(const CZPoseSoA &p_pose, int p_offset) {
	return { wide_load(p_pose.data(CZPoseSoA::ROTATION_X) + p_offset), wide_load(p_pose.data(CZPoseSoA::ROTATION_Y) + p_offset),
		wide_load(p_pose.data(CZPoseSoA::ROTATION_Z) + p_offset), wide_load(p_pose.data(CZPoseSoA::ROTATION_W) + p_offset) };
}

_FORCE_INLINE_ void store_rotation(CZPoseSoA &r_pose, int p_offset, const WideQuaternion &p_value) {
	wide_store(r_pose.data(CZPoseSoA::ROTATION_X) + p_offset, p_value.x);
	wide_store(r_pose.data(CZPoseSoA::ROTATION_Y) + p_offset, p_value.y);
	wide_store(r_pose.data(CZPoseSoA::ROTATION_Z) + p_offset, p_value.z);
	wide_store(r_pose.data(CZPoseSoA::ROTATION_W) + p_offset, p_value.w);
}

_FORCE_INLINE_ PoseWide rotation_dot(const WideQuaternion &p_a, const WideQuaternion &p_b) {
	// Keep Quaternion::dot's left-to-right accumulation and separate operations.
	return wide_add(wide_add(wide_add(wide_mul(p_a.x, p_b.x), wide_mul(p_a.y, p_b.y)), wide_mul(p_a.z, p_b.z)), wide_mul(p_a.w, p_b.w));
}

_FORCE_INLINE_ WideQuaternion rotation_multiply(const WideQuaternion &p_a, const WideQuaternion &p_b) {
	// The exact term order is Quaternion::operator*=, independently per lane.
	return {
		wide_sub(wide_add(wide_add(wide_mul(p_a.w, p_b.x), wide_mul(p_a.x, p_b.w)), wide_mul(p_a.y, p_b.z)), wide_mul(p_a.z, p_b.y)),
		wide_sub(wide_add(wide_add(wide_mul(p_a.w, p_b.y), wide_mul(p_a.y, p_b.w)), wide_mul(p_a.z, p_b.x)), wide_mul(p_a.x, p_b.z)),
		wide_sub(wide_add(wide_add(wide_mul(p_a.w, p_b.z), wide_mul(p_a.z, p_b.w)), wide_mul(p_a.x, p_b.y)), wide_mul(p_a.y, p_b.x)),
		wide_sub(wide_sub(wide_sub(wide_mul(p_a.w, p_b.w), wide_mul(p_a.x, p_b.x)), wide_mul(p_a.y, p_b.y)), wide_mul(p_a.z, p_b.z))
	};
}

_FORCE_INLINE_ WideQuaternion rotation_normalize(const WideQuaternion &p_value) {
	// Godot normalizes by multiplying with 1/length, without an rsqrt estimate.
	const PoseWide inverse_length = wide_div(wide_splat(1), wide_sqrt(rotation_dot(p_value, p_value)));
	return { wide_mul(p_value.x, inverse_length), wide_mul(p_value.y, inverse_length), wide_mul(p_value.z, inverse_length), wide_mul(p_value.w, inverse_length) };
}

WideQuaternion rotation_slerp(const WideQuaternion &p_from, const WideQuaternion &p_to, const float *p_weights) {
	float cosines[4], coefficients_from[4], coefficients_to[4];
	wide_store(cosines, rotation_dot(p_from, p_to));
	for (int lane = 0; lane < 4; lane++) {
		real_t cosine = cosines[lane];
		const bool negative = cosine < 0;
		if (negative) {
			cosine = -cosine;
		}
		real_t scale_from, scale_to;
		const real_t weight = p_weights[lane];
		// Use precisely Quaternion::slerp's scalar trig and near-angle branch.
		if ((1.0f - cosine) > (real_t)CMP_EPSILON) {
			const real_t omega = Math::acos(cosine);
			const real_t sine = Math::sin(omega);
			scale_from = Math::sin((1.0 - weight) * omega) / sine;
			scale_to = Math::sin(weight * omega) / sine;
		} else {
			scale_from = 1.0f - weight;
			scale_to = weight;
		}
		coefficients_from[lane] = scale_from;
		coefficients_to[lane] = negative ? -scale_to : scale_to;
	}
	const PoseWide from = wide_load(coefficients_from);
	const PoseWide to = wide_load(coefficients_to);
	return { wide_add(wide_mul(from, p_from.x), wide_mul(to, p_to.x)), wide_add(wide_mul(from, p_from.y), wide_mul(to, p_to.y)),
		wide_add(wide_mul(from, p_from.z), wide_mul(to, p_to.z)), wide_add(wide_mul(from, p_from.w), wide_mul(to, p_to.w)) };
}

bool normalized_rotations(const CZPoseSoA &p_a, const CZPoseSoA &p_b, const CZPoseSoA *p_rest, int p_start) {
#ifdef MATH_CHECKS
	for (int bone = p_start; bone < MIN(p_start + 4, p_a.size()); bone++) {
		if (!p_a.get_rotation(bone).is_normalized() || !p_b.get_rotation(bone).is_normalized() || (p_rest && !p_rest->get_rotation(bone).is_normalized())) {
			return false; // Preserve the official scalar diagnostics for bad inputs.
		}
	}
#endif
	return true;
}

void blend_rotations(CZPoseSoA &r_result, const CZPoseSoA &p_sample, const CZPoseSoA &p_rest, const real_t *p_weights, bool p_rest_base) {
	const PoseWide zero = wide_splat(0);
	const WideQuaternion identity = { zero, zero, zero, wide_splat(1) };
	const CZPoseSoA &base = p_rest_base ? p_rest : r_result;
	for (int start = 0; start < r_result.size(); start += 4) {
		bool any_active = false;
		bool active[4];
		for (int lane = 0; lane < 4; lane++) {
			active[lane] = !Math::is_zero_approx(p_weights[start + lane]);
			any_active |= active[lane];
		}
		if (!any_active) {
			if (p_rest_base) {
				store_rotation(r_result, start, load_rotation(p_rest, start));
			}
			continue;
		}
		if (!normalized_rotations(base, p_sample, &p_rest, start)) {
			for (int bone = start; bone < MIN(start + 4, r_result.size()); bone++) {
				if (!Math::is_zero_approx(p_weights[bone])) {
					r_result.set_rotation(bone, Animation::interpolate_via_rest(base.get_rotation(bone), p_sample.get_rotation(bone), p_weights[bone], p_rest.get_rotation(bone)));
				} else if (p_rest_base) {
					r_result.set_rotation(bone, p_rest.get_rotation(bone));
				}
			}
			continue;
		}
		WideQuaternion inverse_rest = load_rotation(p_rest, start);
		inverse_rest.x = wide_negate(inverse_rest.x);
		inverse_rest.y = wide_negate(inverse_rest.y);
		inverse_rest.z = wide_negate(inverse_rest.z);
		const WideQuaternion delta = rotation_multiply(inverse_rest, load_rotation(p_sample, start));
		const WideQuaternion from = load_rotation(base, start);
		const WideQuaternion result = rotation_normalize(rotation_multiply(from, rotation_slerp(identity, delta, p_weights + start)));
		// Zero-weight lanes must retain their exact base components, including
		// quaternion sign. This also preserves initialized neutral tail padding.
		store_rotation(r_result, start, { wide_select(from.x, result.x, active), wide_select(from.y, result.y, active), wide_select(from.z, result.z, active), wide_select(from.w, result.w, active) });
	}
}

void interpolate_rotations(CZPoseSoA &r_result, const CZPoseSoA &p_from, const CZPoseSoA &p_to, real_t p_weight) {
	const float weights[4] = { p_weight, p_weight, p_weight, p_weight };
	for (int start = 0; start < r_result.size(); start += 4) {
		if (!normalized_rotations(p_from, p_to, nullptr, start)) {
			for (int bone = start; bone < MIN(start + 4, r_result.size()); bone++) {
				r_result.set_rotation(bone, p_from.get_rotation(bone).slerp(p_to.get_rotation(bone), p_weight));
			}
			continue;
		}
		store_rotation(r_result, start, rotation_slerp(load_rotation(p_from, start), load_rotation(p_to, start), weights));
	}
}
#endif // CZ_POSE_SSE2 || CZ_POSE_NEON

void blend_component(real_t *r_result, const real_t *p_sample, const real_t *p_rest, const real_t *p_weights, int p_size, bool p_simd, bool p_rest_base) {
	int i = 0;
	const real_t *base = p_rest_base ? p_rest : r_result;
#ifdef CZ_POSE_SSE2
	if (p_simd) {
		for (; i < p_size; i += 4) {
			const __m128 delta = _mm_sub_ps(_mm_loadu_ps(p_sample + i), _mm_loadu_ps(p_rest + i));
			const __m128 value = _mm_add_ps(_mm_loadu_ps(base + i), _mm_mul_ps(delta, _mm_loadu_ps(p_weights + i)));
			_mm_storeu_ps(r_result + i, value);
		}
	}
#elif defined(CZ_POSE_NEON)
	if (p_simd) {
		for (; i < p_size; i += 4) {
			const float32x4_t delta = vsubq_f32(vld1q_f32(p_sample + i), vld1q_f32(p_rest + i));
			// Separate multiply/add, matching the scalar/SSE2 rounding contract.
			vst1q_f32(r_result + i, vaddq_f32(vld1q_f32(base + i), vmulq_f32(delta, vld1q_f32(p_weights + i))));
		}
	}
#endif
	for (; i < p_size; i++) {
		r_result[i] = base[i] + (p_sample[i] - p_rest[i]) * p_weights[i];
	}
}

void interpolate_component(real_t *r_result, const real_t *p_from, const real_t *p_to, real_t p_weight, int p_size, bool p_simd) {
	int i = 0;
#ifdef CZ_POSE_SSE2
	if (p_simd) {
		const __m128 weight = _mm_set1_ps(p_weight);
		for (; i < p_size; i += 4) {
			const __m128 from = _mm_loadu_ps(p_from + i);
			const __m128 value = _mm_add_ps(from, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(p_to + i), from), weight));
			_mm_storeu_ps(r_result + i, value);
		}
	}
#elif defined(CZ_POSE_NEON)
	if (p_simd) {
		const float32x4_t weight = vdupq_n_f32(p_weight);
		for (; i < p_size; i += 4) {
			const float32x4_t from = vld1q_f32(p_from + i);
			vst1q_f32(r_result + i, vaddq_f32(from, vmulq_f32(vsubq_f32(vld1q_f32(p_to + i), from), weight)));
		}
	}
#endif
	for (; i < p_size; i++) {
		r_result[i] = p_from[i] + (p_to[i] - p_from[i]) * p_weight;
	}
}

} // namespace

CZPoseSoA::Layout CZPoseSoA::get_default_layout() {
	static const Layout default_layout = OS::get_singleton()->get_environment("CZ_ANIM_POSE_AOS") == "1" ? AOS : SOA;
	return default_layout;
}

void CZPoseSoA::resize(int p_bone_count) {
	ERR_FAIL_COND(p_bone_count < 0);
	bone_count = p_bone_count;
	if (layout == AOS) {
		bone_poses.resize(bone_count);
		for (BonePose &pose : bone_poses) {
			pose = BonePose();
		}
		return;
	}
	for (int component = 0; component < COMPONENT_COUNT; component++) {
		components[component].resize(padded_size());
		const real_t neutral = component == ROTATION_W || component >= SCALE_X ? 1 : 0;
		for (real_t &value : components[component]) {
			value = neutral;
		}
	}
}

void CZPoseSoA::copy_from(const CZPoseSoA &p_source) {
	ERR_FAIL_COND(layout != p_source.layout);
	if (bone_count != p_source.bone_count) {
		resize(p_source.bone_count);
	}
	if (padded_size() == 0 || this == &p_source) {
		return;
	}
	if (layout == AOS) {
		memcpy(bone_poses.ptr(), p_source.bone_poses.ptr(), bone_count * sizeof(BonePose));
		copy_bytes += uint64_t(bone_count) * sizeof(BonePose);
		return;
	}
	for (int component = 0; component < COMPONENT_COUNT; component++) {
		memcpy(components[component].ptr(), p_source.components[component].ptr(), padded_size() * sizeof(real_t));
	}
	copy_bytes += uint64_t(padded_size()) * COMPONENT_COUNT * sizeof(real_t);
}

void CZPoseSoA::set_position(int p_bone, const Vector3 &p_value) {
	ERR_FAIL_INDEX(p_bone, bone_count);
	if (layout == AOS) {
		bone_poses[p_bone].position = p_value;
		return;
	}
	for (int axis = 0; axis < 3; axis++) {
		components[POSITION_X + axis][p_bone] = p_value[axis];
	}
}

void CZPoseSoA::set_rotation(int p_bone, const Quaternion &p_value) {
	ERR_FAIL_INDEX(p_bone, bone_count);
	if (layout == AOS) {
		bone_poses[p_bone].rotation = p_value;
		return;
	}
	for (int axis = 0; axis < 4; axis++) {
		components[ROTATION_X + axis][p_bone] = p_value[axis];
	}
}

void CZPoseSoA::set_scale(int p_bone, const Vector3 &p_value) {
	ERR_FAIL_INDEX(p_bone, bone_count);
	if (layout == AOS) {
		bone_poses[p_bone].scale = p_value;
		return;
	}
	for (int axis = 0; axis < 3; axis++) {
		components[SCALE_X + axis][p_bone] = p_value[axis];
	}
}

Vector3 CZPoseSoA::get_position(int p_bone) const {
	ERR_FAIL_INDEX_V(p_bone, bone_count, Vector3());
	if (layout == AOS) {
		return bone_poses[p_bone].position;
	}
	return Vector3(components[POSITION_X][p_bone], components[POSITION_Y][p_bone], components[POSITION_Z][p_bone]);
}

Quaternion CZPoseSoA::get_rotation(int p_bone) const {
	ERR_FAIL_INDEX_V(p_bone, bone_count, Quaternion());
	if (layout == AOS) {
		return bone_poses[p_bone].rotation;
	}
	return Quaternion(components[ROTATION_X][p_bone], components[ROTATION_Y][p_bone], components[ROTATION_Z][p_bone], components[ROTATION_W][p_bone]);
}

Vector3 CZPoseSoA::get_scale(int p_bone) const {
	ERR_FAIL_INDEX_V(p_bone, bone_count, Vector3(1, 1, 1));
	if (layout == AOS) {
		return bone_poses[p_bone].scale;
	}
	return Vector3(components[SCALE_X][p_bone], components[SCALE_Y][p_bone], components[SCALE_Z][p_bone]);
}

Transform3D CZPoseSoA::get_transform(int p_bone) const {
	return Transform3D(Basis(get_rotation(p_bone)).scaled_local(get_scale(p_bone)), get_position(p_bone));
}

void CZPoseSoA::blend(const CZPoseSoA &p_sample, const CZPoseSoA &p_rest,
		const real_t *p_position_weights, const real_t *p_rotation_weights, const real_t *p_scale_weights, bool p_simd, bool p_rest_base) {
	ERR_FAIL_COND(p_sample.layout != layout || p_rest.layout != layout);
	ERR_FAIL_COND(p_sample.size() != size() || p_rest.size() != size());
	if (layout == AOS) {
		for (int i = 0; i < bone_count; i++) {
			BonePose &result = bone_poses[i];
			const BonePose &sample = p_sample.bone_poses[i];
			const BonePose &rest = p_rest.bone_poses[i];
			const BonePose &base = p_rest_base ? rest : result;
			for (int axis = 0; axis < 3; axis++) {
				result.position[axis] = base.position[axis] + (sample.position[axis] - rest.position[axis]) * p_position_weights[i];
				result.scale[axis] = base.scale[axis] + (sample.scale[axis] - rest.scale[axis]) * p_scale_weights[i];
			}
			if (!Math::is_zero_approx(p_rotation_weights[i])) {
				result.rotation = Animation::interpolate_via_rest(base.rotation, sample.rotation, p_rotation_weights[i], rest.rotation);
			} else if (p_rest_base) {
				result.rotation = rest.rotation;
			}
		}
		return;
	}
	for (int axis = 0; axis < 3; axis++) {
		blend_component(components[POSITION_X + axis].ptr(), p_sample.components[POSITION_X + axis].ptr(), p_rest.components[POSITION_X + axis].ptr(), p_position_weights, padded_size(), p_simd, p_rest_base);
		blend_component(components[SCALE_X + axis].ptr(), p_sample.components[SCALE_X + axis].ptr(), p_rest.components[SCALE_X + axis].ptr(), p_scale_weights, padded_size(), p_simd, p_rest_base);
	}
#if defined(CZ_POSE_SSE2) || defined(CZ_POSE_NEON)
	if (p_simd) {
		blend_rotations(*this, p_sample, p_rest, p_rotation_weights, p_rest_base);
		return;
	}
#endif
	for (int i = 0; i < bone_count; i++) {
		if (!Math::is_zero_approx(p_rotation_weights[i])) {
			const Quaternion base = p_rest_base ? p_rest.get_rotation(i) : get_rotation(i);
			set_rotation(i, Animation::interpolate_via_rest(base, p_sample.get_rotation(i), p_rotation_weights[i], p_rest.get_rotation(i)));
		} else if (p_rest_base) {
			set_rotation(i, p_rest.get_rotation(i));
		}
	}
}

void CZPoseSoA::interpolate(const CZPoseSoA &p_from, const CZPoseSoA &p_to, real_t p_weight, bool p_simd) {
	ERR_FAIL_COND(p_from.layout != layout || p_to.layout != layout);
	ERR_FAIL_COND(p_from.size() != p_to.size());
	if (size() != p_from.size()) {
		resize(p_from.size());
	}
	if (layout == AOS) {
		for (int i = 0; i < bone_count; i++) {
			BonePose &result = bone_poses[i];
			const BonePose &from = p_from.bone_poses[i];
			const BonePose &to = p_to.bone_poses[i];
			for (int axis = 0; axis < 3; axis++) {
				result.position[axis] = from.position[axis] + (to.position[axis] - from.position[axis]) * p_weight;
				result.scale[axis] = from.scale[axis] + (to.scale[axis] - from.scale[axis]) * p_weight;
			}
			result.rotation = from.rotation.slerp(to.rotation, p_weight);
		}
		return;
	}
	for (int axis = 0; axis < 3; axis++) {
		interpolate_component(components[POSITION_X + axis].ptr(), p_from.components[POSITION_X + axis].ptr(), p_to.components[POSITION_X + axis].ptr(), p_weight, padded_size(), p_simd);
		interpolate_component(components[SCALE_X + axis].ptr(), p_from.components[SCALE_X + axis].ptr(), p_to.components[SCALE_X + axis].ptr(), p_weight, padded_size(), p_simd);
	}
#if defined(CZ_POSE_SSE2) || defined(CZ_POSE_NEON)
	if (p_simd) {
		interpolate_rotations(*this, p_from, p_to, p_weight);
		return;
	}
#endif
	for (int i = 0; i < bone_count; i++) {
		// Use Godot's slerp including sign selection and its near-angle branch.
		set_rotation(i, p_from.get_rotation(i).slerp(p_to.get_rotation(i), p_weight));
	}
}

bool CZPoseSoA::is_simd_available() {
#if defined(CZ_POSE_SSE2) || defined(CZ_POSE_NEON)
	return true;
#else
	return false;
#endif
}

uint64_t CZPoseSoA::get_storage_bytes() const {
	if (layout == AOS) {
		return uint64_t(bone_poses.get_capacity()) * sizeof(BonePose);
	}
	uint64_t bytes = 0;
	for (int component = 0; component < COMPONENT_COUNT; component++) {
		bytes += uint64_t(components[component].get_capacity()) * sizeof(real_t);
	}
	return bytes;
}

SafeNumeric<uint64_t> CZPoseSamplingScratch::buffer_growths_total;

void CZPoseSamplingScratch::resize(int p_bone_count) {
	ERR_FAIL_COND(p_bone_count < 0);
	const int padded = (p_bone_count + 3) & ~3;
	for (LocalVector<real_t> *weights : { &position_weights, &rotation_weights, &scale_weights }) {
		const uint32_t previous_capacity = weights->get_capacity();
		weights->resize(padded);
		if (weights->get_capacity() > previous_capacity) {
			buffer_growths_total.increment();
		}
	}
}

uint64_t CZPoseSamplingScratch::get_storage_bytes() const {
	return uint64_t(position_weights.get_capacity() + rotation_weights.get_capacity() + scale_weights.get_capacity()) * sizeof(real_t);
}

HashSet<CZPoseTrackBinding::TrackData *> CZPoseTrackBinding::live_track_data;
HashMap<uint32_t, LocalVector<Ref<CZPoseTrackBinding::TrackData>>> CZPoseTrackBinding::shared_track_data;
uint64_t CZPoseTrackBinding::descriptor_builds = 0;
uint64_t CZPoseTrackBinding::descriptor_cache_hits = 0;
uint64_t CZPoseTrackBinding::descriptor_cache_misses = 0;

CZPoseTrackBinding::TrackData::TrackData() {
	DEV_ASSERT(Thread::is_main_thread());
	live_track_data.insert(this);
	descriptor_builds++;
}

CZPoseTrackBinding::TrackData::~TrackData() {
	DEV_ASSERT(Thread::is_main_thread());
	live_track_data.erase(this);
}

uint32_t CZPoseTrackBinding::TrackData::hash() const {
	uint32_t value = hash_murmur3_one_32(compact_bone_count);
	value = hash_murmur3_one_32(maximum_blend_index, value);
	value = hash_murmur3_one_32(blend_indices_valid, value);
	for (const LocalVector<Track> *tracks : { &position_tracks, &rotation_tracks, &scale_tracks }) {
		value = hash_murmur3_one_32(tracks->size(), value);
		for (const Track &track : *tracks) {
			value = hash_murmur3_one_32(track.animation_track, value);
			value = hash_murmur3_one_32(track.compact_bone, value);
			value = hash_murmur3_one_32(track.blend_index, value);
		}
	}
	return hash_fmix32(value);
}

bool CZPoseTrackBinding::TrackData::matches(const TrackData &p_other) const {
	if (compact_bone_count != p_other.compact_bone_count || maximum_blend_index != p_other.maximum_blend_index || blend_indices_valid != p_other.blend_indices_valid) {
		return false;
	}
	const LocalVector<Track> *channels[] = { &position_tracks, &rotation_tracks, &scale_tracks };
	const LocalVector<Track> *other_channels[] = { &p_other.position_tracks, &p_other.rotation_tracks, &p_other.scale_tracks };
	for (int channel = 0; channel < 3; channel++) {
		if (channels[channel]->size() != other_channels[channel]->size()) {
			return false;
		}
		for (uint32_t i = 0; i < channels[channel]->size(); i++) {
			const Track &a = (*channels[channel])[i];
			const Track &b = (*other_channels[channel])[i];
			if (a.animation_track != b.animation_track || a.compact_bone != b.compact_bone || a.blend_index != b.blend_index) {
				return false;
			}
		}
	}
	return true;
}

uint64_t CZPoseTrackBinding::TrackData::get_storage_bytes() const {
	return sizeof(TrackData) + uint64_t(position_tracks.get_capacity() + rotation_tracks.get_capacity() + scale_tracks.get_capacity()) * sizeof(Track);
}

bool CZPoseTrackBinding::is_shared_track_data_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("CZ_ANIM_SHARED_TRACK_DATA") == "1";
	return enabled;
}

bool CZPoseTrackBinding::is_sample_scratch_reuse_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("CZ_ANIM_REUSE_SAMPLE_SCRATCH") == "1";
	return enabled;
}

bool CZPoseTrackBinding::is_in_place_first_blend_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("CZ_ANIM_IN_PLACE_FIRST_BLEND") == "1";
	return enabled;
}

bool CZPoseTrackBinding::is_zero_pose_skip_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("CZ_ANIM_DISABLE_ZERO_POSE_SKIP") != "1";
	return enabled;
}

Dictionary CZPoseTrackBinding::get_track_data_statistics() {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), Dictionary());
	uint64_t bytes = 0;
	for (const TrackData *data : live_track_data) {
		bytes += data->get_storage_bytes();
	}
	Dictionary result;
	result["shared_track_data_enabled"] = is_shared_track_data_enabled();
	result["track_descriptor_live_count"] = live_track_data.size();
	result["track_descriptor_live_bytes"] = bytes;
	result["track_descriptor_cache_hits"] = descriptor_cache_hits;
	result["track_descriptor_cache_misses"] = descriptor_cache_misses;
	result["track_descriptor_builds"] = descriptor_builds;
	return result;
}

void CZPoseTrackBinding::collect_unused_track_data() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	LocalVector<uint32_t> empty;
	for (KeyValue<uint32_t, LocalVector<Ref<TrackData>>> &entry : shared_track_data) {
		for (int i = int(entry.value.size()) - 1; i >= 0; i--) {
			if (entry.value[i]->get_reference_count() == 1) {
				entry.value.remove_at(i);
			}
		}
		if (entry.value.is_empty()) {
			empty.push_back(entry.key);
		}
	}
	for (uint32_t key : empty) {
		shared_track_data.erase(key);
	}
}

void CZPoseTrackBinding::clear_track_data_cache() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	shared_track_data.clear();
}

real_t CZPoseTrackBinding::_weight(const Track &p_track, real_t p_weight, const LocalVector<real_t> *p_track_weights) {
	if (p_track_weights && p_track.compact_bone >= 0 && p_track.blend_index >= 0 && p_track.blend_index < int(p_track_weights->size())) {
		return p_weight * (*p_track_weights)[p_track.blend_index];
	}
	return p_weight;
}

void CZPoseTrackBinding::reset(const Ref<Animation> &p_animation, int p_compact_bone_count) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(p_compact_bone_count < 0);
	animation = p_animation;
	track_data.instantiate();
	track_data->compact_bone_count = p_compact_bone_count;
	if (!is_sample_scratch_reuse_enabled()) {
		sampling_scratch.resize(p_compact_bone_count);
	}
}

void CZPoseTrackBinding::_make_track_data_unique() {
	if (track_data->get_reference_count() == 1) {
		return;
	}
	Ref<TrackData> unique;
	unique.instantiate();
	unique->compact_bone_count = track_data->compact_bone_count;
	unique->maximum_blend_index = track_data->maximum_blend_index;
	unique->blend_indices_valid = track_data->blend_indices_valid;
	unique->position_tracks = track_data->position_tracks;
	unique->rotation_tracks = track_data->rotation_tracks;
	unique->scale_tracks = track_data->scale_tracks;
	track_data = unique;
}

void CZPoseTrackBinding::finalize() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(track_data.is_null());
	if (!is_shared_track_data_enabled()) {
		return;
	}
	LocalVector<Ref<TrackData>> &candidates = shared_track_data[track_data->hash()];
	for (const Ref<TrackData> &candidate : candidates) {
		if (track_data->matches(*candidate.ptr())) {
			descriptor_cache_hits++;
			track_data = candidate;
			return;
		}
	}
	descriptor_cache_misses++;
	candidates.push_back(track_data);
}

void CZPoseTrackBinding::add_track(Animation::TrackType p_type, int p_animation_track, int p_compact_bone, int p_blend_index) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(animation.is_null());
	ERR_FAIL_INDEX(p_animation_track, animation->get_track_count());
	ERR_FAIL_COND(animation->track_get_type(p_animation_track) != p_type);
	ERR_FAIL_INDEX(p_compact_bone, track_data->compact_bone_count);
	_make_track_data_unique();
	Track track;
	track.animation_track = p_animation_track;
	track.compact_bone = p_compact_bone;
	track.blend_index = p_blend_index;
	switch (p_type) {
		case Animation::TYPE_POSITION_3D:
			track_data->position_tracks.push_back(track);
			break;
		case Animation::TYPE_ROTATION_3D:
			track_data->rotation_tracks.push_back(track);
			break;
		case Animation::TYPE_SCALE_3D:
			track_data->scale_tracks.push_back(track);
			break;
		default:
			ERR_FAIL_MSG("Only bone transform tracks belong in a pose binding.");
	}
	track_data->blend_indices_valid &= p_blend_index >= 0;
	track_data->maximum_blend_index = MAX(track_data->maximum_blend_index, p_blend_index);
}

bool CZPoseTrackBinding::sample_and_blend(double p_time, real_t p_weight, const LocalVector<real_t> *p_track_weights,
		real_t p_motion_scale, const CZPoseSoA &p_rest, CZPoseSoA &r_sample, CZPoseSoA &r_blended, bool p_simd,
		bool p_fastpaths, SamplingStatistics *r_statistics, bool p_zero_weights_certified,
		CZPoseSamplingScratch *p_sampling_scratch, bool p_first_contribution) {
	ERR_FAIL_COND_V(animation.is_null(), false);
	const TrackData &data = *track_data.ptr();
	const int active = p_rest.size();
	ERR_FAIL_COND_V(active > data.compact_bone_count || r_blended.size() != active, false);
	ERR_FAIL_COND_V(&r_sample == &p_rest || &r_sample == &r_blended || &r_blended == &p_rest, false);
	const auto record_statistics = [&](bool p_nonzero_weight, bool p_written, bool p_certified) {
		if (r_statistics) {
			r_statistics->calls++;
			r_statistics->zero_weight_calls += !p_nonzero_weight;
			r_statistics->no_written_calls += !p_written;
			r_statistics->empty_binding_calls += data.position_tracks.is_empty() && data.rotation_tracks.is_empty() && data.scale_tracks.is_empty();
			r_statistics->certified_zero_weight_calls += p_certified;
		}
	};
	// Only the bridge may supply this hint, after checking the exact-zero union
	// of every prepared Pose's bone indices in the current owned or borrowed
	// weight view. Borrowed views are consumed only inside Bridge.evaluate.
	// Invalid index fallbacks and non-finite scalar weights retain the old path.
	if (p_fastpaths && p_zero_weights_certified && Math::is_finite(p_weight) && p_track_weights &&
			data.blend_indices_valid && data.maximum_blend_index < int(p_track_weights->size())) {
		record_statistics(false, false, true);
		return false;
	}
	// A detached caller without a Pose keeps a binding-owned fallback; no task
	// can borrow another Pose's or a global/thread-local sampling buffer.
	CZPoseSamplingScratch &scratch = p_sampling_scratch ? *p_sampling_scratch : sampling_scratch;
	const int padded = (active + 3) & ~3;
	if (scratch.position_weights.size() < uint32_t(padded) || scratch.rotation_weights.size() < uint32_t(padded) || scratch.scale_weights.size() < uint32_t(padded)) {
		scratch.resize(active);
	}
	LocalVector<real_t> &position_weights = scratch.position_weights;
	LocalVector<real_t> &rotation_weights = scratch.rotation_weights;
	LocalVector<real_t> &scale_weights = scratch.scale_weights;
	const auto initialize_sample = [&]() {
		r_sample.copy_from(p_rest);
		for (int i = 0; i < padded; i++) {
			position_weights[i] = rotation_weights[i] = scale_weights[i] = 0;
		}
	};
	if (!p_fastpaths) {
		initialize_sample();
	}
	bool written = false;
	bool nonzero_weight = false;
	for (const Track &track : data.position_tracks) {
		if (track.compact_bone >= active || !animation->track_is_enabled(track.animation_track)) {
			continue;
		}
		const real_t weight = _weight(track, p_weight, p_track_weights);
		if (Math::is_zero_approx(weight)) {
			continue;
		}
		if (r_statistics) {
			nonzero_weight = true;
		}
		Vector3 value;
		if (animation->try_position_track_interpolate(track.animation_track, p_time, &value) == OK) {
			if (p_fastpaths && !written) {
				initialize_sample();
			}
			written = true;
			r_sample.set_position(track.compact_bone, value * p_motion_scale);
			position_weights[track.compact_bone] = weight;
		}
	}
	for (const Track &track : data.rotation_tracks) {
		if (track.compact_bone >= active || !animation->track_is_enabled(track.animation_track)) {
			continue;
		}
		const real_t weight = _weight(track, p_weight, p_track_weights);
		if (Math::is_zero_approx(weight)) {
			continue;
		}
		if (r_statistics) {
			nonzero_weight = true;
		}
		Quaternion value;
		if (animation->try_rotation_track_interpolate(track.animation_track, p_time, &value) == OK) {
			if (p_fastpaths && !written) {
				initialize_sample();
			}
			written = true;
			r_sample.set_rotation(track.compact_bone, value);
			rotation_weights[track.compact_bone] = weight;
		}
	}
	for (const Track &track : data.scale_tracks) {
		if (track.compact_bone >= active || !animation->track_is_enabled(track.animation_track)) {
			continue;
		}
		const real_t weight = _weight(track, p_weight, p_track_weights);
		if (Math::is_zero_approx(weight)) {
			continue;
		}
		if (r_statistics) {
			nonzero_weight = true;
		}
		Vector3 value;
		if (animation->try_scale_track_interpolate(track.animation_track, p_time, &value) == OK) {
			if (p_fastpaths && !written) {
				initialize_sample();
			}
			written = true;
			r_sample.set_scale(track.compact_bone, value);
			scale_weights[track.compact_bone] = weight;
		}
	}
	const bool blended = !p_fastpaths || written;
	if (blended) {
		if (p_first_contribution) {
			r_sample.blend(r_sample, p_rest, position_weights.ptr(), rotation_weights.ptr(), scale_weights.ptr(), p_simd, true);
			SWAP(r_blended, r_sample);
		} else {
			r_blended.blend(r_sample, p_rest, position_weights.ptr(), rotation_weights.ptr(), scale_weights.ptr(), p_simd);
		}
	}
	record_statistics(nonzero_weight, written, false);
	return blended;
}
