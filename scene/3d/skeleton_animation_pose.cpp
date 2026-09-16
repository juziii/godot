/**************************************************************************/
/*  skeleton_animation_pose.cpp                                              */
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

#include "skeleton_animation_pose.h"

#include "core/object/class_db.h"
#include "core/os/thread.h"
#include "core/profiling/profiling.h"
#include "scene/3d/physics/physical_bone_simulator_3d.h"
#include "servers/rendering/rendering_server.h"

static Node3D *capture_target(Node *p_owner, const NodePath &p_path, NodePath &r_path, ObjectID &r_id) {
	if (p_path == r_path && r_id.is_valid()) {
		if (auto *target = Object::cast_to<Node3D>(ObjectDB::get_instance(r_id))) {
			return target;
		}
	}
	r_path = p_path;
	auto *target = Object::cast_to<Node3D>(p_owner->get_node_or_null(p_path));
	r_id = target ? target->get_instance_id() : ObjectID();
	return target;
}

bool SkeletonAnimationPose::fail(const String &p_reason) {
	release();
	fallback_reason = p_reason;
	evaluated = false;
	return false;
}

bool SkeletonAnimationPose::capture(Skeleton3D *p_skeleton) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	if (!p_skeleton || !p_skeleton->is_inside_tree()) {
		return fail("Skeleton is outside the scene tree.");
	}
	if (skeleton_id != p_skeleton->get_instance_id()) {
		release();
	}
	fallback_reason = String();
	evaluated = false;
	bool rebuild = binding_version != p_skeleton->animation_pose_binding_version || skeleton_id != p_skeleton->get_instance_id() || skeleton_version != p_skeleton->get_version() || p_skeleton->rest_dirty || p_skeleton->process_order_dirty;
	p_skeleton->force_update_all_dirty_bones();
	p_skeleton->_update_process_order();
	skeleton_id = p_skeleton->get_instance_id();
	skeleton_version = p_skeleton->get_version();
	binding_version = p_skeleton->animation_pose_binding_version;
	world = p_skeleton->get_global_transform();
	world_interpolated = p_skeleton->get_global_transform_interpolated();
	motion_scale = p_skeleton->get_motion_scale();
	show_rest = p_skeleton->is_show_rest_only();
	int count = p_skeleton->get_bone_count();
	if (copy_source.is_valid() && (copy_source_bone < 0 || copy_source_bone >= copy_source->get_bone_count() || copy_target_bone < 0 || copy_target_bone >= count)) {
		return fail("Cross-skeleton pose binding has an invalid bone index.");
	}
	if (ik.configured) {
		for (int bone : { ik.hips, ik.left_upper_arm, ik.left_lower_arm, ik.left_foot, ik.right_foot, ik.left_toes, ik.right_toes, ik.left_upper_leg, ik.left_lower_leg, ik.right_upper_leg, ik.right_lower_leg }) {
			if (bone < 0 || bone >= count) { return fail("IK input contains an invalid bone index."); }
		}
	}
	if (int(bones.size()) != count) {
		bones.resize(count);
		order.resize(count);
		dirty.resize(count);
		scratch_path.reserve(count);
		rebuild = true;
	}
	for (int i = 0; i < count; i++) {
		const Skeleton3D::Bone &source = p_skeleton->bones[i];
#ifndef DISABLE_DEPRECATED
		if (source.global_pose_override_amount >= CMP_EPSILON) {
			return fail("Deprecated global bone pose overrides require synchronous processing.");
		}
#endif
		BonePose &b = bones[i];
		if (rebuild) {
			b.parent = source.parent;
			b.rest = source.rest;
			b.global_rest = source.global_rest;
			b.offset = source.nested_set_offset;
			b.span = source.nested_set_span;
			order[b.offset] = i;
		}
		b.enabled = source.enabled;
		b.skin_scale = source.skin_scale;
		b.local = p_skeleton->get_bone_pose(i);
		b.position = source.pose_position;
		b.rotation = source.pose_rotation;
		b.scale = source.pose_scale;
		b.global = source.global_pose;
		b.modified = false;
		dirty[i] = false;
	}
	p_skeleton->_find_modifiers();
	modifiers.clear();
	int ti = 0, ci = 0;
	for (ObjectID id : p_skeleton->modifiers) {
		auto *mod = Object::cast_to<SkeletonModifier3D>(ObjectDB::get_instance(id));
		if (!mod || !mod->is_active()) {
			continue;
		}
		if (mod->has_connections(SNAME("modification_processed"))) {
			return fail("Modifier callbacks require synchronous processing: " + mod->get_name());
		}
        if (auto *simulator = Object::cast_to<PhysicalBoneSimulator3D>(mod)) {
            if (!simulator->is_simulating_physics()) { continue; }
            return fail("Active physical bone simulation requires synchronous processing.");
        }
		Modifier m;
		m.id = id;
		m.influence = mod->get_influence();
		if (id == aim.modifier_id) {
			if (aim.aim < 0 || aim.hand < 0) {
				continue;
			}
			for (int bone : { aim.aim, aim.hand, aim.hips, aim.chest, aim.upper_chest, aim.left_leg, aim.right_leg }) {
				if (bone < 0 || bone >= count) {
					return fail("Aim input contains an invalid bone index.");
				}
			}
			m.kind = Modifier::AIM;
		} else if (mod->get_script_instance()) {
			return fail("Script skeleton modifier is not configured for detached evaluation: " + mod->get_name());
		} else if (auto *ik = Object::cast_to<TwoBoneIK3D>(mod)) {
			if (m.influence < 1 && ik->get_setting_count() > 1) { return fail("Multi-setting partial influence requires synchronous processing."); }
			m.kind = Modifier::TWO_BONE;
			m.begin = ti;
			for (int i = 0; i < ik->get_setting_count(); i++) {
				int root = ik->get_root_bone(i), middle = ik->get_middle_bone(i), end = ik->get_end_bone(i);
				if (root < 0 || middle < 0 || end < 0) {
					continue;
				}
				if (ti == int(two_bone.size())) {
					two_bone.resize(ti + 1);
				}
				TwoBoneInput &in = two_bone[ti];
				if (in.modifier_id != id || in.setting_index != i) {
					in.target_id = ObjectID();
					in.pole_id = ObjectID();
				}
				auto *target = capture_target(ik, ik->get_target_node(i), in.target_path, in.target_id);
				auto *pole = capture_target(ik, ik->get_pole_node(i), in.pole_path, in.pole_id);
				if (!target || !pole) {
					continue;
				}
				ti++;
				in.settings.root_bone.bone = root;
				in.settings.middle_bone.bone = middle;
				in.settings.end_bone.bone = end;
				in.settings.use_virtual_end = ik->is_using_virtual_end(i);
				in.settings.extend_end_bone = ik->is_end_bone_extended(i);
				in.settings.end_bone_direction = ik->get_end_bone_direction(i);
				in.settings.end_bone_length = ik->get_end_bone_length(i);
				in.settings.pole_direction = ik->get_pole_direction(i);
				in.settings.pole_direction_vector = ik->get_pole_direction_vector(i);
				in.mutable_axes = ik->are_bone_axes_mutable();
				in.modifier_id = id;
				in.setting_index = i;
				if (!in.mutable_axes) {
					ik->_init_joints(p_skeleton, i);
					const auto *native = ik->tb_settings[i];
					if (!native->is_valid()) {
						ti--;
						continue;
					}
					in.settings = *native;
					in.root = *native->root_joint_solver_info;
					in.middle = *native->mid_joint_solver_info;
				}
				in.target = world_interpolated.affine_inverse().xform(target->get_global_transform_interpolated().origin);
				in.pole = world_interpolated.affine_inverse().xform(pole->get_global_transform_interpolated().origin);
				in.target_id = target->get_instance_id();
				in.pole_id = pole->get_instance_id();
			}
			m.count = ti - m.begin;
		} else if (auto *copy = Object::cast_to<CopyTransformModifier3D>(mod)) {
			if (m.influence < 1 && copy->get_setting_count() > 1) { return fail("Multi-setting partial influence requires synchronous processing."); }
			m.kind = Modifier::COPY;
			m.begin = ci;
			for (int i = 0; i < copy->get_setting_count(); i++) {
				if (copy->get_amount(i) <= 0 || copy->get_apply_bone(i) < 0) {
					continue;
				}
				if (ci == int(copies.size())) {
					copies.resize(ci + 1);
				}
				CopyInput &in = copies[ci];
				if (in.owner_id != id || in.setting_index != i) { in.target_id = ObjectID(); }
				in.owner_id = id;
				in.setting_index = i;
				Node3D *target = nullptr;
				if (copy->get_reference_type(i) == BoneConstraint3D::REFERENCE_TYPE_NODE) {
					target = capture_target(copy, copy->get_reference_node(i), in.target_path, in.target_id);
					if (!target) { continue; }
				} else if (copy->get_reference_bone(i) < 0) {
					continue;
				}
				ci++;
				auto &s = in.settings;
				s.apply_bone = copy->get_apply_bone(i);
				s.reference_bone = copy->get_reference_bone(i);
				s.reference_type = copy->get_reference_type(i);
				s.amount = copy->get_amount(i);
				s.copy_flags = copy->get_copy_flags(i);
				s.axis_flags = copy->get_axis_flags(i);
				s.invert_flags = copy->get_invert_flags(i);
				s.global = copy->is_global(i);
				s.relative = copy->is_relative(i);
				s.additive = copy->is_additive(i);
				if (target) {
					in.target_id = target->get_instance_id();
					in.target = world_interpolated.affine_inverse() * target->get_global_transform_interpolated();
				} else {
					in.target_id = ObjectID();
				}
			}
			m.count = ci - m.begin;
		} else {
			return fail("Unsupported skeleton modifier: " + mod->get_class());
		}
		modifiers.push_back(m);
	}
	if (skins.size() != p_skeleton->skin_bindings.size()) {
		skins.resize(p_skeleton->skin_bindings.size());
	}
	int skin_index = 0;
	for (SkinReference *source : p_skeleton->skin_bindings) {
		SkinOutput &out = skins[skin_index++];
		int bind_count = source->skin->get_bind_count();
		bool changed = rebuild || out.skin != source->skin || out.rendering_skeleton != source->skeleton || source->skeleton_version != skeleton_version || int(out.indices.size()) != bind_count;
		if (!changed) { continue; }
		out.skin = source->skin;
		out.rendering_skeleton = source->skeleton;
		out.indices.resize(bind_count);
		out.bind_poses.resize(bind_count);
		out.transforms.resize(bind_count);
		if (int(source->bind_count) != bind_count) {
			RenderingServer::get_singleton()->skeleton_allocate_data(source->skeleton, bind_count);
			source->bind_count = bind_count;
			source->skin_bone_indices.resize(bind_count);
			source->skin_bone_indices_ptrs = source->skin_bone_indices.ptrw();
		}
		for (int i = 0; i < bind_count; i++) {
			StringName name = source->skin->get_bind_name(i);
			int bone = name == StringName() ? source->skin->get_bind_bone(i) : p_skeleton->find_bone(name);
			if (bone < 0 || bone >= count) { return fail("Skin references an invalid bone."); }
			out.indices[i] = bone;
			out.bind_poses[i] = source->skin->get_bind_pose(i);
			source->skin_bone_indices_ptrs[i] = bone;
		}
		source->skeleton_version = skeleton_version;
	}
	if (!owns_callback_mode) {
		saved_callback_mode = p_skeleton->get_modifier_callback_mode_process();
		owns_callback_mode = true;
		p_skeleton->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
	}
	return true;
}

bool SkeletonAnimationPose::capture_current() {
	return capture(Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id)));
}

void SkeletonAnimationPose::release() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	if (owns_callback_mode) {
		if (auto *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id))) {
			skeleton->set_modifier_callback_mode_process(saved_callback_mode);
		}
		owns_callback_mode = false;
	}
}

SkeletonAnimationPose::~SkeletonAnimationPose() {
	release();
}

Transform3D SkeletonAnimationPose::get_base_local_pose(int p_bone) const {
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Transform3D());
	return bones[p_bone].base;
}

Transform3D SkeletonAnimationPose::get_base_global_pose(int p_bone) const {
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Transform3D());
	return bones[p_bone].base_global;
}

void SkeletonAnimationPose::configure_aim(uint64_t p_modifier_id, const Dictionary &p_input) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	aim.modifier_id = ObjectID(p_modifier_id);
	aim.aim = p_input.get("aim_bone", -1);
	aim.hand = p_input.get("hand_bone", -1);
	aim.hips = p_input.get("hips_bone", -1);
	aim.chest = p_input.get("chest_bone", -1);
	aim.upper_chest = p_input.get("upper_chest_bone", -1);
	aim.left_leg = p_input.get("left_leg_bone", -1);
	aim.right_leg = p_input.get("right_leg_bone", -1);
	aim.left_arm = p_input.get("left_arm_bone", -1);
	aim.left_elbow = p_input.get("left_elbow_bone", -1);
	aim.hip_weight = p_input.get("hip_yaw_weight", 0.0);
	aim.hand_to_muzzle = p_input.get("hand_to_muzzle", Transform3D());
	aim.has_grip = p_input.has("hand_to_grip");
	aim.hand_to_grip = p_input.get("hand_to_grip", Transform3D());
	aim.target = p_input.get("target_world", Vector3());
	aim.up = p_input.get("up_world", Vector3(0, 1, 0));
	aim.grip_target_id = ObjectID(uint64_t(p_input.get("grip_target_id", uint64_t(0))));
	aim.elbow_pole_id = ObjectID(uint64_t(p_input.get("elbow_pole_id", uint64_t(0))));
}

void SkeletonAnimationPose::mark_dirty(int p_bone) {
	BonePose &b = bones[p_bone];
	b.modified = true;
	for (int i = b.offset; i < b.offset + b.span; i++) {
		dirty[i] = true;
	}
}

Transform3D SkeletonAnimationPose::get_global_pose(int p_bone) {
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Transform3D());
	BonePose &b = bones[p_bone];
	if (dirty[b.offset]) {
		Transform3D local = b.enabled && !show_rest ? b.local : b.rest;
		b.global = b.parent < 0 ? local : get_global_pose(b.parent) * local;
		dirty[b.offset] = false;
	}
	return b.global;
}

void SkeletonAnimationPose::update_globals() {
	for (int index : order) {
		get_global_pose(index);
	}
}

Transform3D SkeletonAnimationPose::get_local_pose(int p_bone) const {
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Transform3D());
	return bones[p_bone].local;
}

void SkeletonAnimationPose::set_bone_components(int p_bone, const Vector3 &p_position, const Quaternion &p_rotation, const Vector3 &p_scale, int p_mask) {
	ERR_FAIL_INDEX(p_bone, int(bones.size()));
	auto &b = bones[p_bone];
	if (p_mask & 1) { b.position = p_position; }
	if (p_mask & 2) { b.rotation = p_rotation; }
	if (p_mask & 4) { b.scale = p_scale; }
	b.local.origin = b.position;
	b.local.basis.set_quaternion_scale(b.rotation, b.scale);
	mark_dirty(p_bone);
}

void SkeletonAnimationPose::set_bone_pose_position(int p_bone, const Vector3 &p_position) {
	bones[p_bone].position = p_position;
	bones[p_bone].local.origin = p_position;
	mark_dirty(p_bone);
}
void SkeletonAnimationPose::set_bone_pose_rotation(int p_bone, const Quaternion &p_rotation) {
	bones[p_bone].rotation = p_rotation;
	bones[p_bone].local.basis.set_quaternion_scale(p_rotation, bones[p_bone].scale);
	mark_dirty(p_bone);
}
void SkeletonAnimationPose::set_bone_pose_scale(int p_bone, const Vector3 &p_scale) {
	bones[p_bone].scale = p_scale;
	bones[p_bone].local.basis.set_quaternion_scale(bones[p_bone].rotation, p_scale);
	mark_dirty(p_bone);
}
void SkeletonAnimationPose::set_bone_global_pose(int p_bone, const Transform3D &p_pose) {
	int parent = bones[p_bone].parent;
	Transform3D local = parent < 0 ? p_pose : get_global_pose(parent).affine_inverse() * p_pose;
	set_bone_components(p_bone, local.origin, local.basis.get_rotation_quaternion(), local.basis.get_scale(), 7);
}

Transform3D SkeletonAnimationPose::chain_rest(int p_bone, int p_root, bool p_mutable) {
	if (!p_mutable) {
		return bones[p_bone].global_rest;
	}
	Transform3D tr = bones[p_root].global_rest;
	tr.origin = tr.origin - bones[p_root].rest.origin + bones[p_root].local.origin;
	scratch_path.clear();
	for (int bone = p_bone; bone != p_root && bone != -1; bone = bones[bone].parent) {
		scratch_path.push_back(bone);
	}
	for (int i = int(scratch_path.size()) - 1; i >= 0; i--) {
		const auto &b = bones[scratch_path[i]];
		tr = tr * Transform3D(b.rest.basis, b.local.origin);
	}
	return tr;
}

void SkeletonAnimationPose::solve_two_bone(TwoBoneInput &r_input) {
	auto &s = r_input.settings;
	s.root_joint_solver_info = &r_input.root;
	s.mid_joint_solver_info = &r_input.middle;
	if (!r_input.mutable_axes) {
		s.cache_current_joint_rotations(this, r_input.pole);
		TwoBoneIK3D::solve_pose_joints(this, &s, r_input.target, r_input.pole);
		return;
	}
	int end = s.get_end_bone();
	Vector3 axis;
	if (s.end_bone_direction == SkeletonModifier3D::BONE_DIRECTION_FROM_PARENT) {
		axis = bones[end].rest.basis.xform_inv(r_input.mutable_axes ? bones[end].local.origin : bones[end].rest.origin).normalized();
	} else {
		axis = SkeletonModifier3D::get_vector_from_bone_axis(static_cast<SkeletonModifier3D::BoneAxis>(int(s.end_bone_direction)));
	}
	Vector3 global_rest_origin;
	if (s.extend_end_bone && s.end_bone_length > 0 && !axis.is_zero_approx()) {
		s.end_pos = get_global_pose(end).xform(axis * s.end_bone_length);
		global_rest_origin = chain_rest(end, s.root_bone.bone, r_input.mutable_axes).xform(axis * s.end_bone_length);
	} else {
		s.end_pos = get_global_pose(end).origin;
		global_rest_origin = chain_rest(end, s.root_bone.bone, r_input.mutable_axes).origin;
	}
	Vector3 origin = chain_rest(s.middle_bone.bone, s.root_bone.bone, r_input.mutable_axes).origin;
	axis = global_rest_origin - origin;
	global_rest_origin = origin;
	if (axis.is_zero_approx()) {
		return;
	}
	s.mid_pos = get_global_pose(s.middle_bone.bone).origin;
	r_input.middle.forward_vector = bones[s.middle_bone.bone].global_rest.basis.get_rotation_quaternion().xform_inv(axis).normalized();
	r_input.middle.length = axis.length();
	origin = chain_rest(s.root_bone.bone, s.root_bone.bone, r_input.mutable_axes).origin;
	axis = global_rest_origin - origin;
	if (axis.is_zero_approx()) {
		return;
	}
	s.root_pos = get_global_pose(s.root_bone.bone).origin;
	r_input.root.forward_vector = bones[s.root_bone.bone].global_rest.basis.get_rotation_quaternion().xform_inv(axis).normalized();
	r_input.root.length = axis.length();
	s.init_current_joint_rotations(this);
	double length = r_input.root.length + r_input.middle.length;
	s.cached_length_sq = length * length;
	s.cache_current_joint_rotations(this, r_input.pole);
	TwoBoneIK3D::solve_pose_joints(this, &s, r_input.target, r_input.pole);
}

void SkeletonAnimationPose::solve_copy(CopyInput &r_input) {
	auto &s = r_input.settings;
	Transform3D destination;
	Transform3D to_apply_space;
	if (s.reference_type == BoneConstraint3D::REFERENCE_TYPE_NODE) {
		int parent = bones[s.apply_bone].parent;
		destination = parent >= 0 ? get_global_pose(parent).affine_inverse() * r_input.target : r_input.target;
	} else {
		if (s.is_global()) {
			int parent = bones[s.apply_bone].parent;
			to_apply_space = parent >= 0 ? get_global_pose(parent).affine_inverse() : Transform3D();
			destination = to_apply_space * get_global_pose(s.reference_bone);
		} else {
			destination = get_local_pose(s.reference_bone);
		}
		if (s.is_relative()) {
			Transform3D rest = s.is_global() ? to_apply_space * bones[s.reference_bone].global_rest : bones[s.reference_bone].rest;
			Vector3 scale = destination.basis.get_scale() / rest.basis.get_scale();
			destination.basis = rest.basis.get_rotation_quaternion().inverse() * destination.basis.get_rotation_quaternion();
			destination.basis.scale_local(scale);
			destination.origin -= rest.origin;
		}
	}
	CopyTransformModifier3D::copy_pose(this, &s, s.apply_bone, destination, s.amount);
}

void SkeletonAnimationPose::rotate_global(int p_bone, const Quaternion &p_rotation) {
	Transform3D pose = get_global_pose(p_bone);
	pose.basis = Basis(p_rotation) * pose.basis;
	set_bone_global_pose(p_bone, pose);
}

void SkeletonAnimationPose::solve_aim() {
	aim_evaluated = true;
	Vector3 target = world.affine_inverse().xform(aim.target);
	Vector3 up = world.basis.inverse().xform(aim.up).normalized();
	Transform3D left_leg = get_global_pose(aim.left_leg);
	Transform3D right_leg = get_global_pose(aim.right_leg);
	for (int iteration = 0; iteration < 4; iteration++) {
		Transform3D muzzle = get_global_pose(aim.hand) * aim.hand_to_muzzle;
		Vector3 desired = target - muzzle.origin;
		if (desired.length_squared() < 0.000001f) {
			break;
		}
		Vector3 barrel = muzzle.basis.get_column(2).normalized();
		desired.normalize();
		real_t yaw_angle = Math::atan2(up.dot(barrel.cross(desired)), barrel.dot(desired) - barrel.dot(up) * desired.dot(up));
		Quaternion yaw(up, yaw_angle);
		Vector3 pitch_axis = desired.cross(up).normalized();
		Quaternion pitch(pitch_axis, yaw.xform(barrel).signed_angle_to(desired, pitch_axis));
		pitch = (pitch * yaw).normalized() * yaw.inverse();
		real_t weight = (1 - aim.hip_weight) / 3;
		rotate_global(aim.hips, Quaternion(up, yaw_angle * aim.hip_weight));
		rotate_global(aim.aim, Quaternion(up, yaw_angle * weight));
		rotate_global(aim.chest, Quaternion(up, yaw_angle * weight));
		rotate_global(aim.upper_chest, Quaternion(up, yaw_angle * weight));
		rotate_global(aim.aim, pitch);
	}
	set_bone_global_pose(aim.left_leg, left_leg);
	set_bone_global_pose(aim.right_leg, right_leg);
	Transform3D hand = get_global_pose(aim.hand);
	aim.muzzle = world * hand * aim.hand_to_muzzle;
	aim.error_degrees = Math::rad_to_deg(aim.muzzle.basis.get_column(2).normalized().angle_to((aim.target - aim.muzzle.origin).normalized()));
	if (!aim.has_grip) {
		return;
	}
	aim.grip = hand * aim.hand_to_grip;
	bool has_pole = aim.left_arm >= 0 && aim.left_elbow >= 0;
	if (has_pole) {
		Vector3 root = get_global_pose(aim.left_arm).origin;
		Vector3 middle = get_global_pose(aim.left_elbow).origin;
		Vector3 axis = aim.grip.origin - root;
		if (axis.length_squared() < 0.000001f) {
			aim.elbow_pole = middle + Vector3(0, 0, 1);
		} else {
			Vector3 projected = root + axis * ((middle - root).dot(axis) / axis.length_squared());
			Vector3 bend = middle - projected;
			if (bend.length_squared() < 0.000001f) {
				bend = Vector3(0, 0, 1);
			}
			real_t distance = MAX((middle - root).length() + (aim.grip.origin - middle).length(), real_t(0.1));
			aim.elbow_pole = middle + bend.normalized() * distance;
		}
	}
	// Later modifiers consume the freshly corrected grip, matching PawnAimModifier.UpdateGripTarget.
	for (auto &in : two_bone) {
		if (in.target_id == aim.grip_target_id) {
			in.target = aim.grip.origin;
		}
		if (has_pole && in.pole_id == aim.elbow_pole_id) {
			in.pole = aim.elbow_pole;
		}
	}
	for (auto &in : copies) {
		if (in.target_id == aim.grip_target_id) {
			in.target = aim.grip;
		}
	}
}

void SkeletonAnimationPose::generate_grip_target() {
    if (!aim.has_grip || aim.hand < 0 || aim.hand >= int(bones.size())) { return; }
    const Transform3D grip = get_global_pose(aim.hand) * aim.hand_to_grip;
    set_target(aim.grip_target_id, grip);
    if (aim.left_arm >= 0 && aim.left_elbow >= 0) {
        const Vector3 pole = calculate_pole(get_global_pose(aim.left_arm).origin, get_global_pose(aim.left_elbow).origin, grip.origin, Vector3(0, 0, 1));
        set_target(aim.elbow_pole_id, Transform3D(Basis(), pole), true);
    }
}

void SkeletonAnimationPose::evaluate() {
	if (!fallback_reason.is_empty()) {
		return;
	}
	if (copy_source.is_valid()) {
		Transform3D source_rest = copy_source->world * copy_source->get_bone_global_rest(copy_source_bone);
		Transform3D target_rest = world * get_bone_global_rest(copy_target_bone);
		Transform3D rest_offset = source_rest.affine_inverse() * target_rest;
		Transform3D source_pose = copy_source->world * copy_source->get_base_global_pose(copy_source_bone);
		set_bone_global_pose(copy_target_bone, world.affine_inverse() * source_pose * rest_offset);
	}
	update_globals();
	aim_evaluated = false;
	generated_targets.clear();
	{
	GodotProfileZone("Animation.AimIK");
	generate_ik_targets();
	generate_grip_target();
	for (auto &b : bones) {
		b.base = b.local;
		b.base_position = b.position;
		b.base_rotation = b.rotation;
		b.base_scale = b.scale;
		b.base_global = b.global;
	}
	for (const Modifier &m : modifiers) {
		if (m.influence < 1) {
			for (auto &b : bones) {
				b.before_modifier = b.local;
				b.modified = false;
			}
		}
		switch (m.kind) {
			case Modifier::TWO_BONE:
				for (int i = m.begin; i < m.begin + m.count; i++) {
					solve_two_bone(two_bone[i]);
				}
				break;
			case Modifier::COPY:
				for (int i = m.begin; i < m.begin + m.count; i++) {
					solve_copy(copies[i]);
				}
				break;
			case Modifier::AIM:
				solve_aim();
				break;
		}
		if (m.influence < 1) {
			for (int i = 0; i < int(bones.size()); i++) {
				auto &b = bones[i];
				if (b.modified && b.before_modifier != b.local) {
					Transform3D local = b.before_modifier.interpolate_with(b.local, m.influence);
					set_bone_components(i, local.origin, local.basis.get_rotation_quaternion(), local.basis.get_scale(), 7);
				}
			}
		}
		update_globals();
	}
	}
	for (auto &skin : skins) {
		for (uint32_t i = 0; i < skin.indices.size(); i++) {
			const auto &b = bones[skin.indices[i]];
			Transform3D bind = skin.bind_poses[i];
			if (!b.skin_scale.is_equal_approx(Vector3(1, 1, 1))) {
				bind = bind.scaled(b.skin_scale);
			}
			skin.transforms[i] = b.global * bind;
		}
	}
	evaluated = true;
}

bool SkeletonAnimationPose::publish() {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	if (!evaluated) {
		return false;
	}
	for (const auto &target : generated_targets) {
		if (auto *node = Object::cast_to<Node3D>(ObjectDB::get_instance(target.id))) {
			if (target.position_only) { node->set_position(target.transform.origin); }
			else { node->set_transform(target.transform); }
		}
	}
	if (aim_evaluated && aim.has_grip) {
		if (auto *target = Object::cast_to<Node3D>(ObjectDB::get_instance(aim.grip_target_id))) {
			target->set_transform(aim.grip);
		}
		if (aim.left_arm >= 0 && aim.left_elbow >= 0) {
			if (auto *pole = Object::cast_to<Node3D>(ObjectDB::get_instance(aim.elbow_pole_id))) {
				pole->set_position(aim.elbow_pole);
			}
		}
	}
	auto *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id));
	if (!skeleton || !skeleton->_publish_animation_pose(this)) {
		return fail("Skeleton binding changed before animation publication.");
	}
	for (const Modifier &m : modifiers) {
		if (m.kind == Modifier::TWO_BONE) {
			for (int i = m.begin; i < m.begin + m.count; i++) {
				const auto &in = two_bone[i];
				auto *ik = Object::cast_to<TwoBoneIK3D>(ObjectDB::get_instance(in.modifier_id));
				if (ik && !in.mutable_axes && in.setting_index < ik->get_setting_count()) {
					auto *native = ik->tb_settings[in.setting_index];
					if (native->is_valid()) {
						native->root_pos = in.settings.root_pos;
						native->mid_pos = in.settings.mid_pos;
						native->end_pos = in.settings.end_pos;
						*native->root_joint_solver_info = in.root;
						*native->mid_joint_solver_info = in.middle;
					}
				}
			}
		}
		if (auto *mod = ObjectDB::get_instance(m.id)) {
			mod->emit_signal(SNAME("modification_processed"));
		}
	}
	evaluated = false;
	return true;
}

void SkeletonAnimationPose::_bind_methods() {
	ClassDB::bind_method(D_METHOD("capture_current"), &SkeletonAnimationPose::capture_current);
	ClassDB::bind_method(D_METHOD("release"), &SkeletonAnimationPose::release);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &SkeletonAnimationPose::get_bone_count);
	ClassDB::bind_method(D_METHOD("get_base_local_pose", "bone"), &SkeletonAnimationPose::get_base_local_pose);
	ClassDB::bind_method(D_METHOD("get_base_global_pose", "bone"), &SkeletonAnimationPose::get_base_global_pose);
	ClassDB::bind_method(D_METHOD("capture", "skeleton"), &SkeletonAnimationPose::capture);
	ClassDB::bind_method(D_METHOD("configure_copy_source", "source", "source_bone", "target_bone"), &SkeletonAnimationPose::configure_copy_source);
	ClassDB::bind_method(D_METHOD("configure_ik", "input"), &SkeletonAnimationPose::configure_ik);
	ClassDB::bind_method(D_METHOD("configure_aim", "modifier_id", "input"), &SkeletonAnimationPose::configure_aim);
	ClassDB::bind_method(D_METHOD("set_bone_components", "bone", "position", "rotation", "scale", "mask"), &SkeletonAnimationPose::set_bone_components);
	ClassDB::bind_method(D_METHOD("evaluate"), &SkeletonAnimationPose::evaluate);
	ClassDB::bind_method(D_METHOD("publish"), &SkeletonAnimationPose::publish);
	ClassDB::bind_method(D_METHOD("get_global_pose", "bone"), &SkeletonAnimationPose::get_global_pose);
	ClassDB::bind_method(D_METHOD("get_local_pose", "bone"), &SkeletonAnimationPose::get_local_pose);
	ClassDB::bind_method(D_METHOD("get_motion_scale"), &SkeletonAnimationPose::get_motion_scale);
	ClassDB::bind_method(D_METHOD("get_skeleton_instance_id"), &SkeletonAnimationPose::get_skeleton_instance_id);
	ClassDB::bind_method(D_METHOD("get_fallback_reason"), &SkeletonAnimationPose::get_fallback_reason);
	ClassDB::bind_method(D_METHOD("get_aim_muzzle"), &SkeletonAnimationPose::get_aim_muzzle);
	ClassDB::bind_method(D_METHOD("get_aim_error_degrees"), &SkeletonAnimationPose::get_aim_error_degrees);
}

void SkeletonAnimationPose::configure_ik(const Dictionary &p_input) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ik.configured = !p_input.is_empty();
	if (!ik.configured) { return; }
	generated_targets.reserve(7);
	ik.has_hand_target = p_input.get("has_hand_target", false);
	ik.hand_target_world = p_input.get("hand_target_world", Transform3D());
	ik.hand_weight = p_input.get("left_hand_weight", 0.0);
	ik.left_weight = p_input.get("left_foot_weight", 0.0);
	ik.right_weight = p_input.get("right_foot_weight", 0.0);
	ik.hips = p_input.get("hips_bone", -1);
	ik.left_upper_arm = p_input.get("left_upper_arm_bone", -1);
	ik.left_lower_arm = p_input.get("left_lower_arm_bone", -1);
	ik.left_foot = p_input.get("left_foot_bone", -1);
	ik.right_foot = p_input.get("right_foot_bone", -1);
	ik.left_toes = p_input.get("left_toes_bone", -1);
	ik.right_toes = p_input.get("right_toes_bone", -1);
	ik.left_upper_leg = p_input.get("left_upper_leg_bone", -1);
	ik.left_lower_leg = p_input.get("left_lower_leg_bone", -1);
	ik.right_upper_leg = p_input.get("right_upper_leg_bone", -1);
	ik.right_lower_leg = p_input.get("right_lower_leg_bone", -1);
	ik.arm_rest_pole = p_input.get("left_arm_rest_pole", Vector3());
	ik.left_rest_pole = p_input.get("left_leg_rest_pole", Vector3());
	ik.right_rest_pole = p_input.get("right_leg_rest_pole", Vector3());
	ik.left_length = p_input.get("left_leg_length", 0.0);
	ik.right_length = p_input.get("right_leg_length", 0.0);
	ik.reach_epsilon = p_input.get("leg_reach_epsilon", 0.0);
	ik.maximum_slope = p_input.get("maximum_ground_slope_degrees", 0.0);
	ik.sole_offset = p_input.get("sole_offset_cm", 0.0);
	ik.pelvis_drop = p_input.get("maximum_pelvis_drop_cm", 0.0);
	ik.pelvis_raise = p_input.get("maximum_pelvis_raise_cm", 0.0);
	ik.left_ground.hit = p_input.get("left_ground_has_hit", false);
	ik.right_ground.hit = p_input.get("right_ground_has_hit", false);
	ik.left_ground.position = p_input.get("left_ground_position", Vector3());
	ik.left_ground.normal = p_input.get("left_ground_normal", Vector3());
	ik.right_ground.position = p_input.get("right_ground_position", Vector3());
	ik.right_ground.normal = p_input.get("right_ground_normal", Vector3());
	ik.hand_target = ObjectID(uint64_t(p_input.get("hand_target_id", uint64_t(0))));
	ik.elbow_pole = ObjectID(uint64_t(p_input.get("elbow_pole_id", uint64_t(0))));
	ik.pelvis_target = ObjectID(uint64_t(p_input.get("pelvis_target_id", uint64_t(0))));
	ik.left_target = ObjectID(uint64_t(p_input.get("left_foot_target_id", uint64_t(0))));
	ik.left_pole = ObjectID(uint64_t(p_input.get("left_knee_pole_id", uint64_t(0))));
	ik.right_target = ObjectID(uint64_t(p_input.get("right_foot_target_id", uint64_t(0))));
	ik.right_pole = ObjectID(uint64_t(p_input.get("right_knee_pole_id", uint64_t(0))));
}

Vector3 SkeletonAnimationPose::calculate_pole(const Vector3 &p_root, const Vector3 &p_middle, const Vector3 &p_target, const Vector3 &p_rest) {
	Vector3 axis = p_target - p_root;
	if (axis.length_squared() < 0.000001f) {
		return p_middle + p_rest;
	}
	Vector3 projected = p_root + axis * ((p_middle - p_root).dot(axis) / axis.length_squared());
	Vector3 bend = p_middle - projected;
	if (bend.length_squared() < 0.000001f) { bend = p_rest; }
	real_t distance = MAX((p_middle - p_root).length() + (p_target - p_middle).length(), real_t(0.1));
	return p_middle + bend.normalized() * distance;
}

void SkeletonAnimationPose::set_target(ObjectID p_id, const Transform3D &p_target, bool p_position_only) {
	if (!p_id.is_valid()) { return; }
	GeneratedTarget generated;
	generated.id = p_id;
	generated.transform = p_target;
	generated.position_only = p_position_only;
	generated_targets.push_back(generated);
	for (auto &in : two_bone) {
		if (in.target_id == p_id) { in.target = p_target.origin; }
		if (in.pole_id == p_id) { in.pole = p_target.origin; }
	}
	for (auto &in : copies) {
		if (in.target_id == p_id) {
			if (p_position_only) { in.target.origin = p_target.origin; }
			else { in.target = p_target; }
		}
	}
}

bool SkeletonAnimationPose::create_foot_target(const Transform3D &p_pose, const GroundInput &p_ground, int p_toes, Transform3D &r_target) {
	if (!p_ground.hit || p_ground.normal.length_squared() < 0.000001f) { return false; }
	Vector3 normal = p_ground.normal.normalized();
	real_t slope = Math::rad_to_deg(Math::acos(CLAMP(normal.dot(Vector3(0, 1, 0)), real_t(-1), real_t(1))));
	if (slope > ik.maximum_slope) { return false; }
	Transform3D foot_world = world * p_pose;
	Quaternion alignment(Vector3(0, 1, 0), normal);
	foot_world.basis = (Basis(alignment) * foot_world.basis.orthonormalized()).orthonormalized();
	real_t ground_height = p_ground.position.y - (normal.x * (foot_world.origin.x - p_ground.position.x) + normal.z * (foot_world.origin.z - p_ground.position.z)) / normal.y;
	real_t reference_height = (world * bones[p_toes].global_rest).origin.y;
	foot_world.origin += Vector3(0, 1, 0) * (ground_height - reference_height + ik.sole_offset);
	r_target = world.affine_inverse() * foot_world;
	return true;
}

void SkeletonAnimationPose::generate_ik_targets() {
	if (!ik.configured) { return; }
	constexpr real_t blend_epsilon = 0.0001f;
	if (ik.hand_weight > blend_epsilon && ik.has_hand_target) {
		Transform3D target = world.affine_inverse() * ik.hand_target_world;
		set_target(ik.hand_target, target);
		Vector3 pole = calculate_pole(get_global_pose(ik.left_upper_arm).origin, get_global_pose(ik.left_lower_arm).origin, target.origin, ik.arm_rest_pole);
		set_target(ik.elbow_pole, Transform3D(Basis(), pole), true);
	}
	if (MAX(ik.left_weight, ik.right_weight) <= blend_epsilon) { return; }
	Transform3D left_pose = get_global_pose(ik.left_foot);
	Transform3D right_pose = get_global_pose(ik.right_foot);
	Transform3D left, right;
	bool has_left = ik.left_weight > blend_epsilon && create_foot_target(left_pose, ik.left_ground, ik.left_toes, left);
	bool has_right = ik.right_weight > blend_epsilon && create_foot_target(right_pose, ik.right_ground, ik.right_toes, right);
	real_t left_offset = has_left ? (world.xform(left.origin) - world.xform(left_pose.origin)).y : 0;
	real_t right_offset = has_right ? (world.xform(right.origin) - world.xform(right_pose.origin)).y : 0;
	real_t pelvis_offset = has_left && has_right ? MIN(left_offset, right_offset) : has_left ? left_offset : right_offset;
	pelvis_offset = CLAMP(pelvis_offset, -ik.pelvis_drop, ik.pelvis_raise);
	Transform3D hips = get_global_pose(ik.hips);
	Vector3 hips_origin = hips.origin;
	hips.origin = world.affine_inverse().xform(world.xform(hips.origin) + Vector3(0, 1, 0) * pelvis_offset);
	set_target(ik.pelvis_target, hips);
	Vector3 translation = hips.origin - hips_origin;
	auto clamp_reach = [](const Vector3 &root, const Vector3 &target, real_t length, real_t epsilon) {
		Vector3 offset = target - root;
		real_t reach = MAX(length - epsilon, real_t(0));
		return offset.length_squared() <= reach * reach || offset.length_squared() < 0.000001f ? target : root + offset.normalized() * reach;
	};
	if (has_left) {
		Vector3 root = get_global_pose(ik.left_upper_leg).origin + translation;
		Vector3 middle = get_global_pose(ik.left_lower_leg).origin + translation;
		left.origin = clamp_reach(root, left.origin, ik.left_length, ik.reach_epsilon);
		set_target(ik.left_target, left);
		set_target(ik.left_pole, Transform3D(Basis(), calculate_pole(root, middle, left_pose.origin + translation, ik.left_rest_pole)), true);
	}
	if (has_right) {
		Vector3 root = get_global_pose(ik.right_upper_leg).origin + translation;
		Vector3 middle = get_global_pose(ik.right_lower_leg).origin + translation;
		right.origin = clamp_reach(root, right.origin, ik.right_length, ik.reach_epsilon);
		set_target(ik.right_target, right);
		set_target(ik.right_pole, Transform3D(Basis(), calculate_pole(root, middle, right_pose.origin + translation, ik.right_rest_pole)), true);
	}
}

void SkeletonAnimationPose::configure_copy_source(const Ref<SkeletonAnimationPose> &p_source, int p_source_bone, int p_target_bone) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(p_source.ptr() == this);
	copy_source = p_source;
	copy_source_bone = p_source_bone;
	copy_target_bone = p_target_bone;
}
