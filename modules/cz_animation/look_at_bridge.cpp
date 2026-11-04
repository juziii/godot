/**************************************************************************/
/*  look_at_bridge.cpp                                                    */
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

#include "animation_bridge.h"

#include "scene/3d/look_at_modifier_3d.h"
#include "scene/3d/skeleton_3d.h"

CZLookAtSolver CZAnimationBridge::capture_look_at(const LookAtModifier3D *p_modifier) {
	CZLookAtSolver solver;
	solver.forward_axis = p_modifier->forward_axis;
	solver.primary_rotation_axis = p_modifier->primary_rotation_axis;
	solver.secondary_rotation_axis = p_modifier->secondary_rotation_axis;
	solver.use_secondary_rotation = p_modifier->use_secondary_rotation;
	solver.relative = p_modifier->relative;
	solver.use_angle_limitation = p_modifier->use_angle_limitation;
	solver.symmetry_limitation = p_modifier->symmetry_limitation;
	solver.use_rest_for_limitation = p_modifier->use_rest_for_limitation;
	solver.primary_limit_angle = p_modifier->primary_limit_angle;
	solver.primary_damp_threshold = p_modifier->primary_damp_threshold;
	solver.primary_positive_limit_angle = p_modifier->primary_positive_limit_angle;
	solver.primary_positive_damp_threshold = p_modifier->primary_positive_damp_threshold;
	solver.primary_negative_limit_angle = p_modifier->primary_negative_limit_angle;
	solver.primary_negative_damp_threshold = p_modifier->primary_negative_damp_threshold;
	solver.secondary_limit_angle = p_modifier->secondary_limit_angle;
	solver.secondary_damp_threshold = p_modifier->secondary_damp_threshold;
	solver.secondary_positive_limit_angle = p_modifier->secondary_positive_limit_angle;
	solver.secondary_positive_damp_threshold = p_modifier->secondary_positive_damp_threshold;
	solver.secondary_negative_limit_angle = p_modifier->secondary_negative_limit_angle;
	solver.secondary_negative_damp_threshold = p_modifier->secondary_negative_damp_threshold;
	return solver;
}

void CZAnimationBridge::publish_look_at(LookAtModifier3D *p_modifier, const CZLookAtSolver &p_solver) {
	p_modifier->forward_vector = p_solver.forward_vector;
	p_modifier->forward_vector_nrm = p_solver.forward_vector_nrm;
	p_modifier->secondary_rotation_axis = p_solver.secondary_rotation_axis;
	p_modifier->is_within_limitations = p_solver.is_within_limitations;
	p_modifier->prev_q = p_solver.rotation;
	p_modifier->prev_twist = p_solver.twist;
}

bool CZAnimationBridge::process_look_at(LookAtModifier3D *p_modifier, Skeleton3D *p_skeleton) {
	if (!Math::is_zero_approx(p_modifier->duration)) {
		return false;
	}
	const int bone = p_modifier->bone;
	const Transform3D local = p_skeleton->get_bone_pose(bone);
	const Transform3D rest = p_skeleton->get_bone_rest(bone);
	const Transform3D basis_rest = p_modifier->relative ? local : rest;
	const Transform3D world = p_skeleton->get_global_transform_interpolated();
	const int parent = p_skeleton->get_bone_parent(bone);
	Transform3D bone_space = parent >= 0 ? world * p_skeleton->get_bone_global_pose(parent) : world;
	bone_space.translate_local(basis_rest.origin);
	Transform3D origin = bone_space;
	if (p_modifier->origin_from == LookAtModifier3D::ORIGIN_FROM_SPECIFIC_BONE && p_modifier->origin_bone >= 0 && p_modifier->origin_bone < p_skeleton->get_bone_count()) {
		origin = world * p_skeleton->get_bone_global_pose(p_modifier->origin_bone);
	} else if (p_modifier->origin_from == LookAtModifier3D::ORIGIN_FROM_EXTERNAL_NODE) {
		if (Node3D *external = Object::cast_to<Node3D>(p_modifier->get_node_or_null(p_modifier->origin_external_node))) {
			origin = external->get_global_transform_interpolated();
		}
	}
	Node3D *target = Object::cast_to<Node3D>(p_modifier->get_node_or_null(p_modifier->target_node));
	CZLookAtSolver solver = capture_look_at(p_modifier);
	const Quaternion rotation = solver.solve(rest, local, p_skeleton->get_bone_pose_rotation(bone), bone_space,
			origin.translated_local(p_modifier->origin_offset), target != nullptr, target ? target->get_global_transform_interpolated().origin : Vector3());
	p_skeleton->set_bone_pose_rotation(bone, rotation);
	publish_look_at(p_modifier, solver);
	return true;
}
