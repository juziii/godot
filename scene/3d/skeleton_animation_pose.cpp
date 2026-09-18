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
#include "scene/3d/bone_attachment_3d.h"
#include "scene/scene_string_names.h"

#include "core/object/class_db.h"
#include "core/object/callable_mp.h"
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

bool SkeletonAnimationPose::matches_current_inputs() const {
	auto *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id));
	return skeleton && skeleton_version == skeleton->get_version() &&
		binding_version == skeleton->animation_pose_binding_version && input_version == skeleton->animation_pose_input_version;
}

void SkeletonAnimationPose::configure_attachment(const Ref<SkeletonAnimationPose> &p_source, int p_bone, const Transform3D &p_scale_transform, const Transform3D &p_offset, bool p_disable_scale) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	attachment_source = p_source;
	attachment_bone = p_bone;
	attachment_offset = p_offset;
	attachment_scale_transform = p_scale_transform;
	attachment_disable_scale = p_disable_scale;
}

void SkeletonAnimationPose::set_socket_input(int p_index, int p_bone, const Vector3 &p_offset, const Transform3D &p_parent, const Basis &p_marker_basis, bool p_enabled) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(p_index < 0);
	if (uint32_t(p_index) >= sockets.size()) { sockets.resize(p_index + 1); }
	Socket &socket = sockets[p_index];
	socket.dirty |= socket.bone != p_bone || socket.offset != p_offset ||
		!socket.parent_from_skeleton.is_equal_approx(p_parent) || socket.marker_basis != p_marker_basis || socket.enabled != p_enabled;
	socket.bone = p_bone;
	socket.offset = p_offset;
	socket.parent_from_skeleton = p_parent;
	socket.marker_basis = p_marker_basis;
	socket.enabled = p_enabled;
}

Transform3D SkeletonAnimationPose::get_socket_transform(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(sockets.size()), Transform3D());
	return sockets[p_index].result;
}

void SkeletonAnimationPose::evaluate_sockets() {
	GodotProfileZone("Animation.Socket");
	Transform3D socket_world = world;
	if (attachment_source.is_valid() && attachment_bone >= 0 && attachment_bone < attachment_source->get_bone_count()) {
		socket_world = attachment_source->world * attachment_source->get_global_pose(attachment_bone) * attachment_scale_transform;
		if (attachment_disable_scale) { socket_world.basis.orthonormalize(); }
		socket_world = socket_world * attachment_offset;
	}
	for (Socket &socket : sockets) {
		if (!socket.enabled) { continue; }
		const Transform3D bone_pose = socket.bone >= 0 && socket.bone < get_bone_count() ? get_global_pose(socket.bone) : Transform3D();
		const Basis metric = socket_world.basis.transposed() * socket_world.basis;
		if (socket.initialized && !socket.dirty && socket.last_metric.is_equal_approx(metric) && socket.last_bone.is_equal_approx(bone_pose)) { continue; }
		socket.initialized = true;
		socket.dirty = false;
		socket.last_metric = metric;
		socket.last_bone = bone_pose;
		if (socket.bone < 0) {
			socket.result = Transform3D(socket.marker_basis, socket.offset);
		} else if (socket.bone < get_bone_count()) {
			Transform3D bone_world = socket_world * get_global_pose(socket.bone);
			Transform3D parent_world = socket_world * socket.parent_from_skeleton;
			socket.result = parent_world.affine_inverse() * Transform3D(bone_world.basis.orthonormalized(), bone_world.xform(socket.offset));
		}
	}
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
	if (rebuild || input_version != p_skeleton->animation_pose_input_version) { sample_count = 0; }
	input_version = p_skeleton->animation_pose_input_version;
	p_skeleton->force_update_all_dirty_bones();
	p_skeleton->_update_process_order();
	skeleton_id = p_skeleton->get_instance_id();
	skeleton_version = p_skeleton->get_version();
	binding_version = p_skeleton->animation_pose_binding_version;
	world = p_skeleton->get_global_transform();
	world_interpolated = p_skeleton->get_global_transform_interpolated();
	const Transform3D world_inverse = world_interpolated.affine_inverse();
	motion_scale = p_skeleton->get_motion_scale();
	show_rest = p_skeleton->is_show_rest_only();
	int count = p_skeleton->get_bone_count();
	if (copy_source.is_valid() && (copy_source_bone < 0 || copy_source_bone >= copy_source->get_bone_count() || copy_target_bone < 0 || copy_target_bone >= count)) {
		return fail("Cross-skeleton pose binding has an invalid bone index.");
	}
	if (!pawn_pose.validate_ik_bones(count)) {
		return fail("IK input contains an invalid bone index.");
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
		if (!p_skeleton->modifiers_enabled) { break; }
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
		if (pawn_pose.is_aim_modifier(id)) {
			if (!pawn_pose.has_aim_bones()) { continue; }
			if (!pawn_pose.validate_aim_bones(count)) {
				return fail("Aim input contains an invalid bone index.");
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
				in.target = world_inverse.xform(target->get_global_transform_interpolated().origin);
				in.pole = world_inverse.xform(pole->get_global_transform_interpolated().origin);
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
					in.target = world_inverse * target->get_global_transform_interpolated();
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
		out.buffers[0].resize(bind_count * 12);
		out.buffers[1].resize(bind_count * 12);
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

bool SkeletonAnimationPose::supports_interpolation() const {
	auto *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id));
	if (!skeleton || skeleton->has_connections(SceneStringName(pose_updated))) { return false; }
	List<Object::Connection> connections;
	skeleton->get_signal_connection_list(SceneStringName(skeleton_updated), &connections);
	for (const Object::Connection &connection : connections) {
		auto *attachment = Object::cast_to<BoneAttachment3D>(connection.callable.get_object());
		if (!attachment || attachment->get_script_instance() || attachment->get_override_pose() || connection.flags != 0 ||
			connection.callable != callable_mp(attachment, &BoneAttachment3D::on_skeleton_update)) { return false; }
	}
	return true;
}

bool SkeletonAnimationPose::prepare_frame(bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	auto *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id));
	if (!skeleton || !skeleton->is_inside_tree()) { return fail("Skeleton is outside the scene tree."); }
	const bool compatible = p_interval > 0 && supports_interpolation();
	sample_requested = p_sample || p_exact || sample_count == 0 || !compatible ||
		skeleton_version != skeleton->get_version() || binding_version != skeleton->animation_pose_binding_version ||
		input_version != skeleton->animation_pose_input_version;
	display_requested = p_display || p_exact || !compatible;
	frame_time = p_time;
	interpolation_delay = compatible && !p_exact ? p_interval : 0;
	if (p_exact || !compatible || frame_time < sample_times[sample_index]) { sample_count = 0; }
	evaluated = false;
	generated_targets.clear();
	pawn_pose.reset_frame();
	return !sample_requested || capture(skeleton);
}

void SkeletonAnimationPose::store_sample() {
	sample_index ^= 1;
	samples[sample_index].resize(bones.size());
	for (uint32_t i = 0; i < bones.size(); i++) {
		samples[sample_index][i] = { bones[i].position, bones[i].scale, bones[i].rotation };
	}
	sample_times[sample_index] = frame_time;
	sample_count = MIN(sample_count + 1, 2);
}

void SkeletonAnimationPose::interpolate_frame() {
	if (!display_requested || sample_count == 0) { return; }
	GodotProfileZone("Animation.Interpolate");
	const int previous = sample_index ^ 1;
	const double span = sample_times[sample_index] - sample_times[previous];
	real_t weight = 1;
	if (sample_count == 2 && interpolation_delay > 0 && span > 0 && span <= interpolation_delay + 0.000001) {
		weight = CLAMP((frame_time - interpolation_delay - sample_times[previous]) / span, 0.0, 1.0);
	}
	for (int index : order) {
		auto &bone = bones[index];
		const PoseSample &current = samples[sample_index][index];
		bone.position = current.position;
		bone.rotation = current.rotation;
		bone.scale = current.scale;
		if (weight < 1) {
			const PoseSample &before = samples[previous][index];
			bone.position = before.position.lerp(current.position, weight);
			bone.scale = before.scale.lerp(current.scale, weight);
			bone.rotation = before.rotation.slerp(current.rotation, weight);
		}
		bone.local = Transform3D(Basis(bone.rotation).scaled_local(bone.scale), bone.position);
		const Transform3D local = bone.enabled && !show_rest ? bone.local : bone.rest;
		bone.global = bone.parent < 0 ? local : bones[bone.parent].global * local;
		dirty[bone.offset] = false;
	}
	update_skin_buffers();
	evaluated = true;
}

void SkeletonAnimationPose::release() {
	sample_count = 0;
	if (owns_callback_mode) {
		ERR_FAIL_COND(!Thread::is_main_thread());
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
	pawn_pose.reset_frame();
	generated_targets.clear();
	{
	GodotProfileZone("Animation.AimIK");
	pawn_pose.generate_targets();
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
				pawn_pose.solve_aim();
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
	store_sample();
	evaluated = true;
	interpolate_frame();
}

void SkeletonAnimationPose::update_skin_buffers() {
	GodotProfileZone("Animation.SkinBufferPack");
	for (auto &skin : skins) {
		skin.buffer_index ^= 1;
		Vector<float> &buffer = skin.buffers[skin.buffer_index];
		const float *previous = buffer.ptr();
		float *data = buffer.ptrw();
		if (previous && previous != data) { skin_buffer_copies++; }
		for (uint32_t i = 0; i < skin.indices.size(); i++) {
			const auto &b = bones[skin.indices[i]];
			Transform3D bind = skin.bind_poses[i];
			if (!b.skin_scale.is_equal_approx(Vector3(1, 1, 1))) {
				bind = bind.scaled(b.skin_scale);
			}
			const Transform3D transform = b.global * bind;
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++) {
					data[i * 12 + row * 4 + column] = transform.basis.rows[row][column];
				}
				data[i * 12 + row * 4 + 3] = transform.origin[row];
			}
		}
	}
}

bool SkeletonAnimationPose::publish() {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	if (!evaluated) {
		return false;
	}
	{ GodotProfileZone("Animation.PublishTargets");
		for (const auto &target : generated_targets) {
			if (auto *node = Object::cast_to<Node3D>(ObjectDB::get_instance(target.id))) {
				publish_stats.target_writes++;
				if (target.position_only) { node->set_position(target.transform.origin); }
				else { node->set_transform(target.transform); }
			}
		}
		publish_stats.target_writes += pawn_pose.publish_targets();
	}
	{ GodotProfileZone("Animation.PublishSkeleton");
		auto *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id));
		if (!skeleton || !skeleton->_publish_animation_pose(this)) {
			return fail("Skeleton binding changed before animation publication.");
		}
	}
	{ GodotProfileZone("Animation.PublishModifiers"); for (const Modifier &m : modifiers) {
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
			publish_stats.modifier_signals++;
			mod->emit_signal(SNAME("modification_processed"));
		}
	}
	}
	evaluated = false;
	return true;
}

void SkeletonAnimationPose::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure_attachment", "source", "bone", "scale_transform", "offset", "disable_scale"), &SkeletonAnimationPose::configure_attachment);
	ClassDB::bind_method(D_METHOD("set_socket_input", "index", "bone", "offset", "parent", "marker_basis", "enabled"), &SkeletonAnimationPose::set_socket_input);
	ClassDB::bind_method(D_METHOD("get_socket_transform", "index"), &SkeletonAnimationPose::get_socket_transform);
	ClassDB::bind_method(D_METHOD("set_aim_input", "hip_weight", "muzzle", "has_grip", "grip", "target", "up"), &SkeletonAnimationPose::set_aim_input);
	ClassDB::bind_method(D_METHOD("set_ik_hand_input", "has_target", "target", "weights"), &SkeletonAnimationPose::set_ik_hand_input);
	ClassDB::bind_method(D_METHOD("set_ik_ground_input", "left_hit", "left_position", "left_normal", "right_hit", "right_position", "right_normal"), &SkeletonAnimationPose::set_ik_ground_input);
	ClassDB::bind_method(D_METHOD("supports_interpolation"), &SkeletonAnimationPose::supports_interpolation);
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

void SkeletonAnimationPose::configure_copy_source(const Ref<SkeletonAnimationPose> &p_source, int p_source_bone, int p_target_bone) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(p_source.ptr() == this);
	copy_source = p_source;
	copy_source_bone = p_source_bone;
	copy_target_bone = p_target_bone;
}

void SkeletonAnimationPose::configure_aim(uint64_t p_modifier_id, const Dictionary &p_input) {
	pawn_pose.configure_aim(p_modifier_id, p_input);
}

void SkeletonAnimationPose::configure_ik(const Dictionary &p_input) {
	pawn_pose.configure_ik(p_input);
}

void SkeletonAnimationPose::set_aim_input(real_t p_hip_weight, const Transform3D &p_muzzle, bool p_has_grip, const Transform3D &p_grip, const Vector3 &p_target, const Vector3 &p_up) {
	pawn_pose.set_aim_input(p_hip_weight, p_muzzle, p_has_grip, p_grip, p_target, p_up);
}

void SkeletonAnimationPose::set_ik_hand_input(bool p_has_target, const Transform3D &p_target, const Vector3 &p_weights) {
	pawn_pose.set_ik_hand_input(p_has_target, p_target, p_weights);
}

void SkeletonAnimationPose::set_ik_ground_input(bool p_left_hit, const Vector3 &p_left_position, const Vector3 &p_left_normal, bool p_right_hit, const Vector3 &p_right_position, const Vector3 &p_right_normal) {
	pawn_pose.set_ik_ground_input(p_left_hit, p_left_position, p_left_normal, p_right_hit, p_right_position, p_right_normal);
}
