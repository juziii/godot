/**************************************************************************/
/*  bone_rotation_offset_modifier_3d.cpp                                  */
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

#include "bone_rotation_offset_modifier_3d.h"

#include "core/object/class_db.h"

void BoneRotationOffsetModifier3D::_update_targets(Skeleton3D *p_skeleton) {
	targets.resize(bone_names.size());
	int target_count = 0;
	for (int i = 0; i < bone_names.size(); i++) {
		const int bone = p_skeleton->find_bone(bone_names[i]);
		if (bone < 0) {
			continue;
		}
		Target &target = targets.write[target_count++];
		target.bone = bone;
		target.name_index = i;
		target.depth = 0;
		for (int parent = p_skeleton->get_bone_parent(bone); parent >= 0; parent = p_skeleton->get_bone_parent(parent)) {
			target.depth++;
		}
	}
	targets.resize(target_count);
	if (use_initial_global_pose) {
		// Skeleton bone indices are not guaranteed to be topological. Keep this
		// reusable target cache ordered parent before child without frame allocation.
		for (int i = 1; i < targets.size(); i++) {
			const Target target = targets[i];
			int j = i;
			while (j > 0 && targets[j - 1].depth > target.depth) {
				targets.write[j] = targets[j - 1];
				j--;
			}
			targets.write[j] = target;
		}
	}
}

const Vector<BoneRotationOffsetModifier3D::Target> &BoneRotationOffsetModifier3D::_get_targets(Skeleton3D *p_skeleton) {
	const ObjectID skeleton_id = p_skeleton->get_instance_id();
	const uint64_t skeleton_version = p_skeleton->get_version();
	const uint64_t binding_version = p_skeleton->get_animation_pose_binding_version();
	const int bone_count = p_skeleton->get_bone_count();
	if (targets_dirty || targets_skeleton_id != skeleton_id || targets_skeleton_version != skeleton_version ||
			targets_binding_version != binding_version || targets_bone_count != bone_count) {
		_update_targets(p_skeleton);
		targets_dirty = false;
		targets_skeleton_id = skeleton_id;
		targets_skeleton_version = skeleton_version;
		targets_binding_version = binding_version;
		targets_bone_count = bone_count;
	}
	return targets;
}

void BoneRotationOffsetModifier3D::set_bone_names(const PackedStringArray &p_bone_names) {
	if (bone_names == p_bone_names) {
		return;
	}
	bone_names = p_bone_names;
	targets_dirty = true;
	if (is_inside_tree()) {
		_update_skeleton();
	}
}

PackedStringArray BoneRotationOffsetModifier3D::get_bone_names() const {
	return bone_names;
}

void BoneRotationOffsetModifier3D::set_rotation_weights(const PackedFloat32Array &p_weights) {
	if (rotation_weights == p_weights) {
		return;
	}
	rotation_weights = p_weights;
}

PackedFloat32Array BoneRotationOffsetModifier3D::get_rotation_weights() const {
	return rotation_weights;
}

void BoneRotationOffsetModifier3D::set_rotation_offsets(const TypedArray<Quaternion> &p_offsets) {
	_clear_rotation_offset_blend_samples();
	rotation_offsets.resize(p_offsets.size());
	for (int i = 0; i < p_offsets.size(); i++) {
		Quaternion offset = p_offsets[i];
		offset.normalize();
		rotation_offsets.write[i] = offset;
	}
}

void BoneRotationOffsetModifier3D::set_rotation_offsets_packed(const PackedVector4Array &p_offsets) {
	_clear_rotation_offset_blend_samples();
	rotation_offsets.resize(p_offsets.size());
	for (int i = 0; i < p_offsets.size(); i++) {
		const Vector4 packed = p_offsets[i];
		Quaternion offset(packed.x, packed.y, packed.z, packed.w);
		if (!offset.is_finite() || offset.length_squared() <= CMP_EPSILON) {
			offset = Quaternion();
		} else {
			offset.normalize();
		}
		rotation_offsets.write[i] = offset;
	}
}

bool BoneRotationOffsetModifier3D::set_rotation_offset_blend_samples_packed(const PackedVector4Array &p_from, const PackedVector4Array &p_to) {
	if (p_from.size() != p_to.size()) {
		return false;
	}
	rotation_offset_blend_from.resize(p_from.size());
	rotation_offset_blend_to.resize(p_to.size());
	for (int i = 0; i < p_from.size(); i++) {
		const Vector4 from = p_from[i];
		const Vector4 to = p_to[i];
		Quaternion from_rotation(from.x, from.y, from.z, from.w);
		Quaternion to_rotation(to.x, to.y, to.z, to.w);
		if (!from_rotation.is_finite() || from_rotation.length_squared() <= CMP_EPSILON) {
			from_rotation = Quaternion();
		} else {
			from_rotation.normalize();
		}
		if (!to_rotation.is_finite() || to_rotation.length_squared() <= CMP_EPSILON) {
			to_rotation = Quaternion();
		} else {
			to_rotation.normalize();
		}
		rotation_offset_blend_from.write[i] = from_rotation;
		rotation_offset_blend_to.write[i] = to_rotation;
	}
	rotation_offsets.clear();
	has_rotation_offset_blend_samples = true;
	return true;
}

void BoneRotationOffsetModifier3D::set_rotation_offset_blend_weight(float p_weight) {
	rotation_offset_blend_weight = Math::is_finite(p_weight) ? CLAMP(p_weight, 0.0f, 1.0f) : 0.0f;
}

float BoneRotationOffsetModifier3D::get_rotation_offset_blend_weight() const {
	return rotation_offset_blend_weight;
}

void BoneRotationOffsetModifier3D::_clear_rotation_offset_blend_samples() {
	rotation_offset_blend_from.clear();
	rotation_offset_blend_to.clear();
	rotation_offset_blend_weight = 0.0f;
	has_rotation_offset_blend_samples = false;
}

TypedArray<Quaternion> BoneRotationOffsetModifier3D::get_rotation_offsets() const {
	TypedArray<Quaternion> offsets;
	if (has_rotation_offset_blend_samples) {
		for (int i = 0; i < rotation_offset_blend_from.size(); i++) {
			offsets.push_back(rotation_offset_blend_from[i].slerp(rotation_offset_blend_to[i], rotation_offset_blend_weight).normalized());
		}
		return offsets;
	}
	for (int i = 0; i < rotation_offsets.size(); i++) {
		offsets.push_back(rotation_offsets[i]);
	}
	return offsets;
}

void BoneRotationOffsetModifier3D::set_bone_rotation_offset(int p_index, const Quaternion &p_rotation) {
	ERR_FAIL_INDEX(p_index, bone_names.size());
	_clear_rotation_offset_blend_samples();
	if (rotation_offsets.size() <= p_index) {
		const int old_size = rotation_offsets.size();
		rotation_offsets.resize(p_index + 1);
		for (int i = old_size; i < rotation_offsets.size(); i++) {
			rotation_offsets.write[i] = Quaternion();
		}
	}
	Quaternion offset = p_rotation;
	offset.normalize();
	if (rotation_offsets[p_index].is_equal_approx(offset)) {
		return;
	}
	rotation_offsets.write[p_index] = offset;
}

Quaternion BoneRotationOffsetModifier3D::get_bone_rotation_offset(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, bone_names.size(), rotation);
	if (has_rotation_offset_blend_samples && p_index < rotation_offset_blend_from.size()) {
		return rotation_offset_blend_from[p_index].slerp(rotation_offset_blend_to[p_index], rotation_offset_blend_weight).normalized();
	}
	return p_index < rotation_offsets.size() ? rotation_offsets[p_index] : rotation;
}

void BoneRotationOffsetModifier3D::clear_rotation_offsets() {
	if (rotation_offsets.is_empty() && !has_rotation_offset_blend_samples) {
		return;
	}
	rotation_offsets.clear();
	_clear_rotation_offset_blend_samples();
}

void BoneRotationOffsetModifier3D::set_rotation_offset(const Quaternion &p_rotation) {
	_clear_rotation_offset_blend_samples();
	rotation = p_rotation;
	rotation.normalize();
}

Quaternion BoneRotationOffsetModifier3D::get_rotation_offset() const {
	return rotation;
}

void BoneRotationOffsetModifier3D::set_rotation_space(RotationSpace p_space) {
	ERR_FAIL_INDEX(p_space, 2);
	rotation_space = p_space;
}

BoneRotationOffsetModifier3D::RotationSpace BoneRotationOffsetModifier3D::get_rotation_space() const {
	return rotation_space;
}

void BoneRotationOffsetModifier3D::set_use_initial_global_pose(bool p_enabled) {
	if (use_initial_global_pose == p_enabled) {
		return;
	}
	use_initial_global_pose = p_enabled;
	targets_dirty = true;
}

bool BoneRotationOffsetModifier3D::is_using_initial_global_pose() const {
	return use_initial_global_pose;
}

void BoneRotationOffsetModifier3D::_process_modification(double p_delta) {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton || (rotation_offsets.is_empty() && !has_rotation_offset_blend_samples && rotation.is_equal_approx(Quaternion()))) {
		return;
	}
	const Vector<Target> &resolved_targets = _get_targets(skeleton);
	if (use_initial_global_pose) {
		initial_global_rotations.resize(skeleton->get_bone_count());
		for (int i = 0; i < resolved_targets.size(); i++) {
			const int bone = resolved_targets[i].bone;
			initial_global_rotations.write[bone] = skeleton->get_bone_global_pose(bone).basis.get_rotation_quaternion();
		}
	}
	for (int i = 0; i < resolved_targets.size(); i++) {
		const Target &target = resolved_targets[i];
		const Quaternion offset = get_bone_rotation_offset(target.name_index);
		const float weight = (target.name_index < rotation_weights.size() ? rotation_weights[target.name_index] : 1.0f) * get_influence();
		if (use_initial_global_pose) {
			apply_from_initial_global_pose(skeleton, target.bone, offset, CLAMP(weight, 0.0f, 1.0f), initial_global_rotations[target.bone]);
		} else {
			apply(skeleton, target.bone, offset, rotation_space, CLAMP(weight, 0.0f, 1.0f));
		}
	}
}

void BoneRotationOffsetModifier3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bone_names", "bone_names"), &BoneRotationOffsetModifier3D::set_bone_names);
	ClassDB::bind_method(D_METHOD("get_bone_names"), &BoneRotationOffsetModifier3D::get_bone_names);
	ClassDB::bind_method(D_METHOD("set_rotation_weights", "rotation_weights"), &BoneRotationOffsetModifier3D::set_rotation_weights);
	ClassDB::bind_method(D_METHOD("get_rotation_weights"), &BoneRotationOffsetModifier3D::get_rotation_weights);
	ClassDB::bind_method(D_METHOD("set_rotation_offsets", "rotation_offsets"), &BoneRotationOffsetModifier3D::set_rotation_offsets);
	ClassDB::bind_method(D_METHOD("set_rotation_offsets_packed", "rotation_offsets"), &BoneRotationOffsetModifier3D::set_rotation_offsets_packed);
	ClassDB::bind_method(D_METHOD("set_rotation_offset_blend_samples_packed", "from", "to"), &BoneRotationOffsetModifier3D::set_rotation_offset_blend_samples_packed);
	ClassDB::bind_method(D_METHOD("set_rotation_offset_blend_weight", "weight"), &BoneRotationOffsetModifier3D::set_rotation_offset_blend_weight);
	ClassDB::bind_method(D_METHOD("get_rotation_offset_blend_weight"), &BoneRotationOffsetModifier3D::get_rotation_offset_blend_weight);
	ClassDB::bind_method(D_METHOD("get_rotation_offsets"), &BoneRotationOffsetModifier3D::get_rotation_offsets);
	ClassDB::bind_method(D_METHOD("set_bone_rotation_offset", "index", "rotation"), &BoneRotationOffsetModifier3D::set_bone_rotation_offset);
	ClassDB::bind_method(D_METHOD("get_bone_rotation_offset", "index"), &BoneRotationOffsetModifier3D::get_bone_rotation_offset);
	ClassDB::bind_method(D_METHOD("clear_rotation_offsets"), &BoneRotationOffsetModifier3D::clear_rotation_offsets);
	ClassDB::bind_method(D_METHOD("set_rotation_offset", "rotation_offset"), &BoneRotationOffsetModifier3D::set_rotation_offset);
	ClassDB::bind_method(D_METHOD("get_rotation_offset"), &BoneRotationOffsetModifier3D::get_rotation_offset);
	ClassDB::bind_method(D_METHOD("set_rotation_space", "rotation_space"), &BoneRotationOffsetModifier3D::set_rotation_space);
	ClassDB::bind_method(D_METHOD("get_rotation_space"), &BoneRotationOffsetModifier3D::get_rotation_space);
	ClassDB::bind_method(D_METHOD("set_use_initial_global_pose", "enabled"), &BoneRotationOffsetModifier3D::set_use_initial_global_pose);
	ClassDB::bind_method(D_METHOD("is_using_initial_global_pose"), &BoneRotationOffsetModifier3D::is_using_initial_global_pose);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "bone_names"), "set_bone_names", "get_bone_names");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "rotation_weights"), "set_rotation_weights", "get_rotation_weights");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "rotation_offsets", PROPERTY_HINT_ARRAY_TYPE, "Quaternion"), "set_rotation_offsets", "get_rotation_offsets");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rotation_offset_blend_weight", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_rotation_offset_blend_weight", "get_rotation_offset_blend_weight");
	ADD_PROPERTY(PropertyInfo(Variant::QUATERNION, "rotation_offset"), "set_rotation_offset", "get_rotation_offset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "rotation_space", PROPERTY_HINT_ENUM, "Local,Skeleton"), "set_rotation_space", "get_rotation_space");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_initial_global_pose"), "set_use_initial_global_pose", "is_using_initial_global_pose");

	BIND_ENUM_CONSTANT(ROTATION_SPACE_LOCAL);
	BIND_ENUM_CONSTANT(ROTATION_SPACE_SKELETON);
}
