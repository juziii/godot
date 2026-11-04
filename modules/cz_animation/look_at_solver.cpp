/**************************************************************************/
/*  look_at_solver.cpp                                                    */
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

#include "look_at_solver.h"

#include "scene/3d/look_at_modifier_3d.h"

float CZLookAtSolver::remap_damped(float p_from, float p_to, float p_damp_threshold, float p_value) {
	float sign = std::signbit(p_value) ? -1.0f : 1.0f;
	float abs_value = Math::abs(p_value);

	if (Math::is_equal_approx(p_damp_threshold, 1.0f) || Math::is_zero_approx(p_to)) {
		return sign * CLAMP(abs_value, p_from, p_to); // Avoid division by zero.
	}

	float value = Math::inverse_lerp(p_from, p_to, abs_value);

	if (value <= p_damp_threshold) {
		return sign * CLAMP(abs_value, p_from, p_to);
	}

	double limit = Math::PI;
	double inv_to = 1.0 / p_to;
	double end_x = limit * inv_to;
	double position = abs_value * inv_to;
	Vector2 start = Vector2(p_damp_threshold, p_damp_threshold);
	Vector2 mid = Vector2(1.0, 1.0);
	Vector2 end = Vector2(end_x, 1.0);
	value = get_bspline_y(start, mid, end, position);

	return sign * Math::lerp(p_from, p_to, value);
}

double CZLookAtSolver::get_bspline_y(const Vector2 &p_from, const Vector2 &p_control, const Vector2 &p_to, double p_x) {
	double a = p_from.x - 2.0 * p_control.x + p_to.x;
	double b = -2.0 * p_from.x + 2.0 * p_control.x;
	double c = p_from.x - p_x;
	double t = 0.0;
	if (Math::is_zero_approx(a)) {
		t = -c / b; // Almost linear.
	} else {
		double discriminant = b * b - 4.0 * a * c;
		double sqrt_discriminant = Math::sqrt(discriminant);
		double e = 1.0 / (2.0 * a);
		double t1 = (-b + sqrt_discriminant) * e;
		t = (0.0 <= t1 && t1 <= 1.0) ? t1 : (-b - sqrt_discriminant) * e;
	}
	double u = 1.0 - t;
	double y = u * u * p_from.y + 2.0 * u * t * p_control.y + t * t * p_to.y;
	return y;
}

Transform3D CZLookAtSolver::look_at_with_axes(const Transform3D &p_rest) {
	// Primary rotation by projection to 2D plane by xform_inv and picking elements.
	Vector3 current_vector = LookAtModifier3D::get_basis_vector_from_bone_axis(p_rest.basis, forward_axis).normalized();
	Vector2 src_vec2 = LookAtModifier3D::get_projection_vector(p_rest.basis.xform_inv(forward_vector_nrm), primary_rotation_axis).normalized();
	Vector2 dst_vec2 = LookAtModifier3D::get_projection_vector(p_rest.basis.xform_inv(current_vector), primary_rotation_axis).normalized();
	real_t calculated_angle = src_vec2.angle_to(dst_vec2);
	Transform3D primary_result = p_rest.rotated_local(SkeletonModifier3D::get_vector_from_axis(primary_rotation_axis), calculated_angle);
	Transform3D current_result = primary_result; // primary_result will be used by calculation of secondary rotation, current_result is rotated by that.
	float limit_angle = 0.0;
	float damp_threshold = 0.0;

	if (use_angle_limitation) {
		if (symmetry_limitation) {
			limit_angle = primary_limit_angle * 0.5f;
			damp_threshold = primary_damp_threshold;
		} else {
			if (std::signbit(calculated_angle)) {
				limit_angle = primary_negative_limit_angle;
				damp_threshold = primary_negative_damp_threshold;
			} else {
				limit_angle = primary_positive_limit_angle;
				damp_threshold = primary_positive_damp_threshold;
			}
		}
		if (Math::abs(calculated_angle) > limit_angle) {
			is_within_limitations = false;
		}
		calculated_angle = remap_damped(0, limit_angle, damp_threshold, calculated_angle);
		current_result = p_rest.rotated_local(SkeletonModifier3D::get_vector_from_axis(primary_rotation_axis), calculated_angle);
	}

	// Needs for detecting flipping even if use_secondary_rotation is false.
	secondary_rotation_axis = LookAtModifier3D::get_secondary_rotation_axis(forward_axis, primary_rotation_axis);

	if (!use_secondary_rotation) {
		return current_result;
	}

	// Secondary rotation by projection to 2D plane by xform_inv and picking elements.
	current_vector = LookAtModifier3D::get_basis_vector_from_bone_axis(primary_result.basis, forward_axis).normalized();
	src_vec2 = LookAtModifier3D::get_projection_vector(primary_result.basis.xform_inv(forward_vector_nrm), secondary_rotation_axis).normalized();
	dst_vec2 = LookAtModifier3D::get_projection_vector(primary_result.basis.xform_inv(current_vector), secondary_rotation_axis).normalized();
	calculated_angle = src_vec2.angle_to(dst_vec2);

	if (use_angle_limitation) {
		if (symmetry_limitation) {
			limit_angle = secondary_limit_angle * 0.5f;
			damp_threshold = secondary_damp_threshold;
		} else {
			if (std::signbit(calculated_angle)) {
				limit_angle = secondary_negative_limit_angle;
				damp_threshold = secondary_negative_damp_threshold;
			} else {
				limit_angle = secondary_positive_limit_angle;
				damp_threshold = secondary_positive_damp_threshold;
			}
		}
		if (Math::abs(calculated_angle) > limit_angle) {
			is_within_limitations = false;
		}
		calculated_angle = remap_damped(0, limit_angle, damp_threshold, calculated_angle);
	}

	current_result = current_result.rotated_local(SkeletonModifier3D::get_vector_from_axis(secondary_rotation_axis), calculated_angle);

	return current_result;
}

Quaternion CZLookAtSolver::solve(const Transform3D &p_rest, const Transform3D &p_pose, const Quaternion &p_rotation, const Transform3D &p_bone_space, const Transform3D &p_origin, bool p_has_target, const Vector3 &p_target) {
	const Transform3D bone_rest = relative ? p_pose : p_rest;
	rotation = p_rotation;
	twist = Quaternion();
	forward_vector = Vector3();
	forward_vector_nrm = Vector3();
	is_within_limitations = true;
	Quaternion rest_rotation;
	if (use_rest_for_limitation) {
		rest_rotation = p_rest.basis.get_rotation_quaternion();
		Vector3 axis = SkeletonModifier3D::get_vector_from_bone_axis(forward_axis);
		Quaternion pose_from_rest = (rest_rotation.inverse() * bone_rest.basis.get_rotation_quaternion()).normalized();
		twist = Quaternion(axis, SkeletonModifier3D::get_roll_angle(pose_from_rest, axis));
	}
	if (!p_has_target) {
		return rotation;
	}
	forward_vector = p_bone_space.orthonormalized().basis.xform_inv(p_target - p_origin.origin);
	if (forward_vector.is_zero_approx()) {
		return rotation;
	}
	forward_vector_nrm = forward_vector.normalized();
	if (forward_vector_nrm.abs().is_equal_approx(SkeletonModifier3D::get_vector_from_axis(primary_rotation_axis))) {
		forward_vector = Vector3();
		return rotation;
	}
	if (use_rest_for_limitation) {
		Transform3D rest_transform = bone_rest;
		rest_transform.basis = Basis(rest_rotation);
		rotation = (look_at_with_axes(rest_transform).basis.get_rotation_quaternion() * twist).normalized();
		forward_vector = rest_rotation.xform_inv(forward_vector_nrm);
	} else {
		rotation = look_at_with_axes(bone_rest).basis.get_rotation_quaternion();
		forward_vector = bone_rest.basis.xform_inv(forward_vector_nrm);
	}
	return rotation;
}
