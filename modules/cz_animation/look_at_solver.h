/**************************************************************************/
/*  look_at_solver.h                                                      */
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

#include "scene/3d/skeleton_modifier_3d.h"

struct CZLookAtSolver {
	using BoneAxis = SkeletonModifier3D::BoneAxis;
	BoneAxis forward_axis = SkeletonModifier3D::BONE_AXIS_PLUS_Z;
	Vector3::Axis primary_rotation_axis = Vector3::AXIS_Y;
	Vector3::Axis secondary_rotation_axis = Vector3::AXIS_X;
	bool use_secondary_rotation = true;
	bool relative = false;
	bool use_angle_limitation = false;
	bool symmetry_limitation = true;
	bool use_rest_for_limitation = false;
	float primary_limit_angle = Math::TAU;
	float primary_damp_threshold = 1.0f;
	float primary_positive_limit_angle = Math::PI;
	float primary_positive_damp_threshold = 1.0f;
	float primary_negative_limit_angle = Math::PI;
	float primary_negative_damp_threshold = 1.0f;
	float secondary_limit_angle = Math::TAU;
	float secondary_damp_threshold = 1.0f;
	float secondary_positive_limit_angle = Math::PI;
	float secondary_positive_damp_threshold = 1.0f;
	float secondary_negative_limit_angle = Math::PI;
	float secondary_negative_damp_threshold = 1.0f;
	Vector3 forward_vector;
	Vector3 forward_vector_nrm;
	Quaternion rotation;
	Quaternion twist;
	bool is_within_limitations = true;

	static float remap_damped(float p_from, float p_to, float p_damp_threshold, float p_value);
	static double get_bspline_y(const Vector2 &p_from, const Vector2 &p_control, const Vector2 &p_to, double p_x);
	Transform3D look_at_with_axes(const Transform3D &p_rest);
	Quaternion solve(const Transform3D &p_rest, const Transform3D &p_pose, const Quaternion &p_rotation, const Transform3D &p_bone_space, const Transform3D &p_origin, bool p_has_target, const Vector3 &p_target);
};
