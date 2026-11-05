/**************************************************************************/
/*  skeleton_animation_pose.cpp                                           */
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

#include "animation_batch_processor.h"
#include "animation_bridge.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/profiling/profiling.h"
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/look_at_modifier_3d.h"
#include "scene/3d/skeleton_modifier_3d.h"
#include "servers/rendering/rendering_server.h"
#if !defined(PHYSICS_3D_DISABLED)
#include "scene/3d/physics/physical_bone_simulator_3d.h"
#endif

bool SkeletonAnimationPose::global_simd_enabled = true;
bool SkeletonAnimationPose::global_bone_lod_enabled = true;
Mutex SkeletonAnimationPose::cleanup_mutex;
LocalVector<SkeletonAnimationPose::Cleanup *> SkeletonAnimationPose::deferred_cleanup;

void SkeletonAnimationPose::set_simd_enabled(bool p_enabled) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	if (global_simd_enabled != p_enabled) {
		CZAnimationBridge::synchronize();
		global_simd_enabled = p_enabled;
	}
}

void SkeletonAnimationPose::set_bone_lod_enabled(bool p_enabled) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	if (global_bone_lod_enabled != p_enabled) {
		CZAnimationBridge::synchronize();
		global_bone_lod_enabled = p_enabled;
	}
}

bool SkeletonAnimationPose::_fail(const String &p_reason) {
	fallback_reason = p_reason;
	evaluated = false;
	return false;
}

static Node3D *capture_target(Node *p_owner, const NodePath &p_path, NodePath &r_path, ObjectID &r_id) {
	if (p_path == r_path && r_id.is_valid()) {
		if (Node3D *target = ObjectDB::get_instance<Node3D>(r_id)) {
			return target;
		}
	}
	r_path = p_path;
	Node3D *target = Object::cast_to<Node3D>(p_owner->get_node_or_null(p_path));
	r_id = target ? target->get_instance_id() : ObjectID();
	return target;
}

bool SkeletonAnimationPose::capture(Skeleton3D *p_skeleton) {
	return _capture(p_skeleton, false);
}

bool SkeletonAnimationPose::_capture(Skeleton3D *p_skeleton, bool p_animated) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	Ref<SkeletonAnimationPose> keep_alive(this);
	const uint64_t generation = ++capture_generation;
	const ObjectID target_id = p_skeleton ? p_skeleton->get_instance_id() : ObjectID();
	if (!CZAnimationBridge::is_processing_pose(this)) {
		CZAnimationBridge::synchronize();
	}
	if (capture_generation != generation) {
		return false;
	}
	return CZAnimationBridge::capture_skeleton(this, ObjectDB::get_instance<Skeleton3D>(target_id), p_animated);
}

bool CZAnimationBridge::capture_skeleton(SkeletonAnimationPose *p_pose, Skeleton3D *p_skeleton, bool p_animated) {
	if (!p_skeleton || !p_skeleton->is_inside_tree()) {
		return p_pose->_fail("Skeleton is outside the scene tree.");
	}
	if (p_pose->skeleton_id != p_skeleton->get_instance_id()) {
		p_pose->_release_capture();
		p_pose->binding_dirty = true;
	}
	const ObjectID id = p_skeleton->get_instance_id();
	const uint64_t generation = p_pose->capture_generation;
	p_pose->skeleton_id = id;
	if (!p_pose->observed) {
		observe_skeleton(p_skeleton);
		p_pose->observed = true;
	}
	p_pose->fallback_reason = String();
	p_pose->display_valid = false;
#ifndef DISABLE_DEPRECATED
	// Updating native globals consumes non-persistent overrides. Detect them
	// before that callout, only on the invalidated binding/layout slow path.
	if (p_pose->binding_dirty || p_pose->binding_version != skeleton_version(p_skeleton) ||
			p_pose->structural_version != p_skeleton->version || p_pose->bones.size() != p_skeleton->bones.size()) {
		for (const Skeleton3D::Bone &bone : p_skeleton->bones) {
			if (bone.global_pose_override_amount >= CMP_EPSILON) {
				return p_pose->_fail("Deprecated global pose override requires synchronous processing.");
			}
		}
	}
#endif
	p_skeleton->force_update_all_dirty_bones();
	if (p_pose->capture_generation != generation) {
		return false;
	}
	if (ObjectDB::get_instance(id) != p_skeleton) {
		return p_pose->_fail("Skeleton was deleted during capture.");
	}
	p_skeleton->_update_process_order();
	if (p_pose->capture_generation != generation) {
		return false;
	}
	if (ObjectDB::get_instance(id) != p_skeleton) {
		return p_pose->_fail("Skeleton was deleted during hierarchy update.");
	}
	const int count = p_skeleton->bones.size();
	const uint64_t revision = skeleton_version(p_skeleton);
	const int desired_lod = SkeletonAnimationPose::global_bone_lod_enabled ? p_pose->requested_lod : 0;
	const bool binding_rebuild = p_pose->binding_dirty || p_pose->binding_version != revision ||
			p_pose->structural_version != p_skeleton->version || p_pose->bones.size() != uint32_t(count);
	const bool rebuild = binding_rebuild || p_pose->bound_lod != desired_lod || p_pose->bound_force_full_bones != p_pose->force_full_bones;
	p_pose->skeleton_id = p_skeleton->get_instance_id();
	const uint64_t input_version = skeleton_input_version(p_skeleton);
	const bool input_changed = p_pose->input_version != input_version;
	p_pose->input_version = input_version;
	p_pose->world = p_skeleton->get_global_transform();
	p_pose->world_interpolated = p_skeleton->get_global_transform_interpolated();
	p_pose->motion_scale = p_skeleton->motion_scale;
	p_pose->show_rest = p_skeleton->show_rest_only;
	p_pose->simd_enabled = SkeletonAnimationPose::global_simd_enabled;
	if (p_pose->show_rest) {
		return p_pose->_fail("Show-rest mode requires synchronous processing.");
	}
	int modifier_count = 0;
	p_pose->required_next = p_pose->configured_required;
	for (const KeyValue<int, int> &required : p_pose->external_requirements) {
		if (!p_pose->required_next.has(required.key)) {
			p_pose->required_next.push_back(required.key);
		}
	}
	if (p_pose->copy_target_bone >= 0 && !p_pose->required_next.has(p_pose->copy_target_bone)) {
		p_pose->required_next.push_back(p_pose->copy_target_bone);
	}
	for (const SkeletonAnimationPose::Socket &socket : p_pose->sockets) {
		if (socket.enabled && socket.bone >= 0 && !p_pose->required_next.has(socket.bone)) {
			p_pose->required_next.push_back(socket.bone);
		}
	}
	p_skeleton->_find_modifiers();
	if (!p_pose->pawn_pose.validate_ik_bones(count)) {
		return p_pose->_fail("IK input contains an invalid bone index.");
	}
	if (modifiers_enabled(p_skeleton)) {
		for (ObjectID id : p_skeleton->modifiers) {
			SkeletonModifier3D *modifier = Object::cast_to<SkeletonModifier3D>(ObjectDB::get_instance(id));
			if (!modifier || !modifier->is_active()) {
				continue;
			}
			if (modifier->has_connections(SNAME("modification_processed"))) {
				return p_pose->_fail("Modifier callbacks require synchronous processing: " + modifier->get_name());
			}
#ifndef PHYSICS_3D_DISABLED
			if (PhysicalBoneSimulator3D *simulator = Object::cast_to<PhysicalBoneSimulator3D>(modifier)) {
				if (!simulator->is_simulating_physics()) {
					continue;
				}
			}
#endif
			if (modifier_count == int(p_pose->modifiers.size())) {
				p_pose->modifiers.resize(modifier_count + 1);
			}
			SkeletonAnimationPose::Modifier &value = p_pose->modifiers[modifier_count];
			const bool changed_modifier = value.id != id;
			if (changed_modifier) {
				value.look_target_id = ObjectID();
				value.origin_target_id = ObjectID();
			}
			value.id = id;
			value.influence = modifier->get_influence();
			value.aim = false;
			value.look_at = false;
			value.copy = false;
			value.two_bone = false;
			value.has_target = false;
			value.has_external_origin = false;
			value.targets.clear();
			value.rotations.clear();
			value.weights.clear();
			if (p_pose->pawn_pose.is_aim_modifier(id)) {
				if (!p_pose->pawn_pose.has_aim_bones()) {
					continue;
				}
				if (!p_pose->pawn_pose.validate_aim_bones(count)) {
					return p_pose->_fail("Aim input contains an invalid bone index.");
				}
				value.aim = true;
			} else if (!modifier->get_script_instance()) {
				const StringName type = modifier->get_class_name();
				if (type != SNAME("BoneRotationOffsetModifier3D") && type != SNAME("LookAtModifier3D") &&
						type != SNAME("CopyTransformModifier3D") && type != SNAME("TwoBoneIK3D")) {
					return p_pose->_fail("Custom native modifier requires synchronous processing: " + String(type));
				}
				BoneRotationOffsetModifier3D *offset = Object::cast_to<BoneRotationOffsetModifier3D>(modifier);
				LookAtModifier3D *look_at = Object::cast_to<LookAtModifier3D>(modifier);
				CopyTransformModifier3D *copy = Object::cast_to<CopyTransformModifier3D>(modifier);
				TwoBoneIK3D *two_bone = Object::cast_to<TwoBoneIK3D>(modifier);
				if (two_bone) {
					value.two_bone = true;
					int ik_count = 0;
					for (int setting = 0; setting < two_bone->get_setting_count(); setting++) {
						const auto *native = two_bone->tb_settings[setting];
						if (native->root_bone.bone < 0 || native->middle_bone.bone < 0 || !native->is_end_valid()) {
							continue;
						}
						two_bone->_init_joints(p_skeleton, setting);
						if (!native->is_valid()) {
							continue;
						}
						if (ik_count == int(value.two_bones.size())) {
							value.two_bones.resize(ik_count + 1);
						}
						SkeletonAnimationPose::TwoBoneInput &input = value.two_bones[ik_count];
						if (changed_modifier || input.setting_index != setting) {
							input.target_id = ObjectID();
							input.pole_id = ObjectID();
						}
						Node3D *target = capture_target(two_bone, native->target_node, input.target_path, input.target_id);
						Node3D *pole = capture_target(two_bone, native->pole_node, input.pole_path, input.pole_id);
						if (!target || !pole) {
							continue;
						}
						input.settings = *native;
						input.root = *native->root_joint_solver_info;
						input.middle = *native->mid_joint_solver_info;
						// Never carry native solver pointers into an asynchronous job.
						// The worker rebinds these two pointers to the owned values above.
						input.settings.root_joint_solver_info = nullptr;
						input.settings.mid_joint_solver_info = nullptr;
						input.setting_index = setting;
						input.mutable_axes = two_bone->are_bone_axes_mutable();
						input.target = p_pose->world_interpolated.affine_inverse().xform(target->get_global_transform_interpolated().origin);
						input.pole = p_pose->world_interpolated.affine_inverse().xform(pole->get_global_transform_interpolated().origin);
						for (int bone : { native->root_bone.bone, native->middle_bone.bone, native->get_end_bone() }) {
							if (bone >= 0 && bone < count && !p_pose->required_next.has(bone)) {
								p_pose->required_next.push_back(bone);
							}
						}
						ik_count++;
					}
					value.two_bones.resize(ik_count);
				} else if (copy) {
					value.copy = true;
					int copy_count = 0;
					for (int setting = 0; setting < copy->get_setting_count(); setting++) {
						const int apply_bone = copy->get_apply_bone(setting);
						if (copy->get_amount(setting) <= 0 || apply_bone < 0 || apply_bone >= count) {
							continue;
						}
						if (copy_count == int(value.copies.size())) {
							value.copies.resize(copy_count + 1);
						}
						SkeletonAnimationPose::CopyInput &input = value.copies[copy_count];
						if (changed_modifier) {
							input.target_id = ObjectID();
							input.target_path = NodePath();
						}
						auto &settings = input.settings;
						settings.apply_bone = apply_bone;
						settings.reference_bone = copy->get_reference_bone(setting);
						settings.reference_type = copy->get_reference_type(setting);
						settings.amount = copy->get_amount(setting);
						settings.copy_flags = copy->get_copy_flags(setting);
						settings.axis_flags = copy->get_axis_flags(setting);
						settings.invert_flags = copy->get_invert_flags(setting);
						settings.global = copy->is_global(setting);
						settings.relative = copy->is_relative(setting);
						settings.additive = copy->is_additive(setting);
						if (settings.reference_type == BoneConstraint3D::REFERENCE_TYPE_NODE) {
							Node3D *target = capture_target(copy, copy->get_reference_node(setting), input.target_path, input.target_id);
							if (!target) {
								continue;
							}
							input.target = p_pose->world_interpolated.affine_inverse() * target->get_global_transform_interpolated();
						} else if (settings.reference_bone < 0 || settings.reference_bone >= count) {
							continue;
						}
						for (int bone : { settings.apply_bone, settings.reference_bone }) {
							if (bone >= 0 && bone < count && !p_pose->required_next.has(bone)) {
								p_pose->required_next.push_back(bone);
							}
						}
						copy_count++;
					}
					value.copies.resize(copy_count);
				} else if (look_at) {
					if (!Math::is_zero_approx(look_at->duration)) {
						return p_pose->_fail("LookAtModifier3D time interpolation requires synchronous processing (duration must be zero).");
					}
					if (look_at->bone < 0 || look_at->bone >= count) {
						continue;
					}
					value.look_at = true;
					value.look_solver = capture_look_at(look_at);
					value.look_bone = look_at->bone;
					value.origin_bone = look_at->origin_bone;
					value.origin_from = look_at->origin_from;
					value.origin_offset = look_at->origin_offset;
					if (Node3D *target = capture_target(look_at, look_at->target_node, value.look_target_path, value.look_target_id)) {
						value.has_target = true;
						value.target = target->get_global_transform_interpolated().origin;
					}
					if (look_at->origin_from == LookAtModifier3D::ORIGIN_FROM_EXTERNAL_NODE) {
						if (Node3D *origin = capture_target(look_at, look_at->origin_external_node, value.origin_target_path, value.origin_target_id)) {
							value.has_external_origin = true;
							value.external_origin = origin->get_global_transform_interpolated();
						}
					}
					for (int bone : { value.look_bone, value.origin_bone }) {
						if (bone >= 0 && bone < count && !p_pose->required_next.has(bone)) {
							p_pose->required_next.push_back(bone);
						}
					}
				} else if (!offset) {
					return p_pose->_fail("Modifier has no detached solver: " + modifier->get_class());
				} else {
					value.space = offset->get_rotation_space();
					value.initial_global = offset->is_using_initial_global_pose();
					const Vector<BoneRotationOffsetModifier3D::Target> &targets = offset->_get_targets(p_skeleton);
					const PackedFloat32Array weights = offset->get_rotation_weights();
					for (const BoneRotationOffsetModifier3D::Target &target : targets) {
						value.targets.push_back(target.bone);
						value.rotations.push_back(offset->get_bone_rotation_offset(target.name_index));
						value.weights.push_back(target.name_index < weights.size() ? weights[target.name_index] : 1);
						if (!p_pose->required_next.has(target.bone)) {
							p_pose->required_next.push_back(target.bone);
						}
					}
				}
			} else {
				return p_pose->_fail("Script modifier is not configured for detached evaluation: " + modifier->get_name());
			}
			modifier_count++;
		}
	}
	p_pose->modifiers.resize(modifier_count);
	List<Object::Connection> connections;
	p_skeleton->get_signal_connection_list(SNAME("skeleton_updated"), &connections);
	for (const Object::Connection &connection : connections) {
		BoneAttachment3D *attachment = Object::cast_to<BoneAttachment3D>(connection.callable.get_object());
		if (attachment && attachment->get_bone_idx() >= 0 && !p_pose->required_next.has(attachment->get_bone_idx())) {
			p_pose->required_next.push_back(attachment->get_bone_idx());
		}
	}
	bool dependencies_changed = p_pose->required_next.size() != p_pose->required.size();
	for (uint32_t index = 0; !dependencies_changed && index < p_pose->required.size(); index++) {
		dependencies_changed = p_pose->required_next[index] != p_pose->required[index];
	}
	if (dependencies_changed) {
		p_pose->required = p_pose->required_next;
		p_pose->binding_dirty = true;
	}
	const bool rebind_tracks = binding_rebuild || p_pose->binding_dirty;
	const bool rebound = rebuild || p_pose->binding_dirty;
	if (rebound) {
		if (rebind_tracks) {
			p_pose->bones.resize(count);
			p_pose->parents.resize(count);
			p_pose->rest_transforms.resize(count);
			p_pose->first_disabled.resize(count);
			for (int bone = 0; bone < count; bone++) {
				const Skeleton3D::Bone &source = p_skeleton->bones[bone];
#ifndef DISABLE_DEPRECATED
				if (source.global_pose_override_amount >= CMP_EPSILON) {
					return p_pose->_fail("Deprecated global pose override requires synchronous processing.");
				}
#endif
				p_pose->bones[bone] = { source.parent, source.rest, source.global_rest, source.skin_scale, source.enabled };
				p_pose->parents[bone] = source.parent;
				p_pose->rest_transforms[bone] = source.rest;
				p_pose->first_disabled[bone] = 0;
			}
			for (int index = 0; index < p_pose->lod_names.size(); index++) {
				const int bone = p_skeleton->find_bone(p_pose->lod_names[index]);
				if (bone >= 0) {
					p_pose->first_disabled[bone] = p_pose->lod_levels[index];
				}
			}
		}
		if (!p_pose->lod.build(p_pose->parents, p_pose->rest_transforms, p_pose->first_disabled,
					p_pose->required, desired_lod, p_pose->force_full_bones)) {
			return p_pose->_fail("Invalid bone hierarchy or LOD dependencies.");
		}
		const int active = p_pose->lod.get_active_bone_count();
		p_pose->reference_pose.resize(active);
		p_pose->reference_mask.resize(active);
		p_pose->sampled.resize(active);
		p_pose->blended.resize(active);
		p_pose->displayed.resize(active);
		p_pose->displayed_base.resize(active);
		p_pose->before_modifier.resize(active);
		p_pose->history[0].resize(active);
		p_pose->history[1].resize(active);
		p_pose->base_history[0].resize(active);
		p_pose->base_history[1].resize(active);
		p_pose->scratch_path.reserve(count);
		p_pose->initial_global_rotations.reserve(count);
		for (int compact = 0; compact < active; compact++) {
			p_pose->reference_mask[compact] = 0;
			const Skeleton3D::Bone &source = p_skeleton->bones[p_pose->lod.to_original(compact)];
			p_pose->reference_pose.set_position(compact, source.enabled ? source.pose_position : source.rest.origin);
			p_pose->reference_pose.set_rotation(compact, source.enabled ? source.pose_rotation : source.rest.basis.get_rotation_quaternion());
			p_pose->reference_pose.set_scale(compact, source.enabled ? source.pose_scale : source.rest.basis.get_scale());
		}
		p_pose->captured_bones += active;
		p_pose->capture_rebounds_total++;
		p_pose->blended.copy_from(p_pose->reference_pose);
		p_pose->retained_base.copy_from(p_pose->blended);
		p_pose->base_pose.copy_from(p_pose->reference_pose);
		p_pose->displayed.copy_from(p_pose->reference_pose);
		p_pose->history_count = 0;
		p_pose->layout_version += rebind_tracks;
		p_pose->bound_lod = desired_lod;
		p_pose->bound_force_full_bones = p_pose->force_full_bones;
		p_pose->binding_version = revision;
		p_pose->structural_version = p_skeleton->version;
		p_pose->binding_dirty = false;
	}
	// Animated frames are sampled directly into the existing SoA reference. Only
	// binding/input changes need another native component snapshot; standalone
	// Capture always exposes the current native pose immediately.
	if (!rebound && (!p_animated || input_changed || p_pose->reference_capture_dirty)) {
		p_pose->capture_input_refreshes_total++;
		for (int compact = 0; compact < p_pose->reference_pose.size(); compact++) {
			const Skeleton3D::Bone &source = p_skeleton->bones[p_pose->lod.to_original(compact)];
			p_pose->blended.set_position(compact, source.enabled ? source.pose_position : source.rest.origin);
			p_pose->blended.set_rotation(compact, source.enabled ? source.pose_rotation : source.rest.basis.get_rotation_quaternion());
			p_pose->blended.set_scale(compact, source.enabled ? source.pose_scale : source.rest.basis.get_scale());
			if (!(p_pose->reference_mask[compact] & 1)) {
				p_pose->reference_pose.set_position(compact, source.enabled ? source.pose_position : source.rest.origin);
			}
			if (!(p_pose->reference_mask[compact] & 2)) {
				p_pose->reference_pose.set_rotation(compact, source.enabled ? source.pose_rotation : source.rest.basis.get_rotation_quaternion());
			}
			if (!(p_pose->reference_mask[compact] & 4)) {
				p_pose->reference_pose.set_scale(compact, source.enabled ? source.pose_scale : source.rest.basis.get_scale());
			}
		}
		p_pose->captured_bones += p_pose->reference_pose.size();
		p_pose->retained_base.copy_from(p_pose->blended);
	}
	p_pose->reference_capture_dirty = false;
	if (p_pose->copy_source.is_valid() && (p_pose->copy_source_bone < 0 || p_pose->copy_source_bone >= p_pose->copy_source->get_bone_count() || p_pose->copy_target_bone < 0 || p_pose->copy_target_bone >= count)) {
		return p_pose->_fail("Invalid cross-skeleton copy binding.");
	}
	p_pose->skins.resize(p_skeleton->skin_bindings.size());
	int skin_index = 0;
	for (SkinReference *reference : p_skeleton->skin_bindings) {
		SkeletonAnimationPose::SkinOutput &skin = p_pose->skins[skin_index++];
		const int binds = reference->skin->get_bind_count();
		if (!rebuild && skin.skin == reference->skin && skin.rid == reference->skeleton && skin.bones.size() == uint32_t(binds)) {
			continue;
		}
		skin.skin = reference->skin;
		skin.rid = reference->skeleton;
		skin.bones.resize(binds);
		skin.bind_poses.resize(binds);
		skin.buffer.resize(binds * 12);
		if (reference->bind_count != uint32_t(binds)) {
			RenderingServer::get_singleton()->skeleton_allocate_data(reference->skeleton, binds);
			reference->bind_count = binds;
			reference->skin_bone_indices.resize(binds);
			reference->skin_bone_indices_ptrs = reference->skin_bone_indices.ptrw();
		}
		for (int bind = 0; bind < binds; bind++) {
			const StringName name = skin.skin->get_bind_name(bind);
			const int bone = name == StringName() ? skin.skin->get_bind_bone(bind) : p_skeleton->find_bone(name);
			if (bone < 0 || bone >= count) {
				return p_pose->_fail("Skin references an invalid skeleton bone.");
			}
			skin.bones[bind] = bone;
			skin.bind_poses[bind] = skin.skin->get_bind_pose(bind);
			reference->skin_bone_indices_ptrs[bind] = bone;
		}
		reference->skeleton_version = p_skeleton->version;
	}
	if (!p_pose->owns_callback) {
		acquire_skeleton_control(p_skeleton);
		p_pose->owns_callback = true;
	}
	if (!p_animated) {
		p_pose->_update_globals();
		p_pose->base_pose.copy_from(p_pose->blended);
		p_pose->base_globals = p_pose->globals;
	}
	return true;
}

bool SkeletonAnimationPose::capture_current() {
	return capture(ObjectDB::get_instance<Skeleton3D>(skeleton_id));
}

bool SkeletonAnimationPose::matches_current_inputs() const {
	Skeleton3D *skeleton = ObjectDB::get_instance<Skeleton3D>(skeleton_id);
	return skeleton && skeleton->get_version() == structural_version && CZAnimationBridge::skeleton_version(skeleton) == binding_version &&
			CZAnimationBridge::skeleton_input_version(skeleton) == input_version && !binding_dirty && !reference_capture_dirty;
}

void SkeletonAnimationPose::configure_bone_lod(const PackedStringArray &p_bone_names, const PackedInt32Array &p_first_disabled_lod) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	ERR_FAIL_COND(p_bone_names.size() != p_first_disabled_lod.size());
	HashSet<String> names;
	for (int i = 0; i < p_bone_names.size(); i++) {
		ERR_FAIL_COND(p_bone_names[i].is_empty() || names.has(p_bone_names[i]) || p_first_disabled_lod[i] < 1);
		names.insert(p_bone_names[i]);
	}
	CZAnimationBridge::synchronize();
	capture_generation++;
	lod_names = p_bone_names;
	lod_levels = p_first_disabled_lod;
	binding_dirty = true;
	history_count = 0;
}

void SkeletonAnimationPose::set_bone_lod(int p_level) {
	ERR_FAIL_COND(!Thread::is_main_thread() || p_level < 0);
	if (requested_lod != p_level) {
		Ref<SkeletonAnimationPose> keep_alive(this);
		CZAnimationBridge::synchronize();
		capture_generation++;
		requested_lod = p_level;
		history_count = 0;
	}
}

bool SkeletonAnimationPose::supports_interpolation() const {
	Skeleton3D *skeleton = ObjectDB::get_instance<Skeleton3D>(skeleton_id);
	if (!skeleton || skeleton->has_connections(SNAME("pose_updated"))) {
		return false;
	}
	List<Object::Connection> signal_connections;
	skeleton->get_signal_connection_list(SNAME("skeleton_updated"), &signal_connections);
	for (const Object::Connection &connection : signal_connections) {
		BoneAttachment3D *attachment = Object::cast_to<BoneAttachment3D>(connection.callable.get_object());
		if (!attachment || attachment->get_script_instance() || attachment->get_override_pose() || connection.flags != 0 ||
				connection.callable != callable_mp(attachment, &BoneAttachment3D::on_skeleton_update)) {
			return false;
		}
	}
	return true;
}

bool SkeletonAnimationPose::prepare_frame(bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval, bool p_force_full_bones) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	sample_blend_us = interpolate_us = skin_pack_us = matrix_us = 0;
	captured_bones = 0;
	simd_enabled = global_simd_enabled;
	const int desired_lod = global_bone_lod_enabled ? requested_lod : 0;
	// At LOD0 all bones are already present. Exact firing requests must not
	// invalidate the same full binding when toggling ForceFullBones.
	force_full_bones = p_force_full_bones && desired_lod > 0;
	const bool changed = !matches_current_inputs() || force_full_bones != bound_force_full_bones || desired_lod != bound_lod;
	const bool compatible = p_interval > 0 && supports_interpolation();
	sample_requested = p_sample || p_exact || changed || history_count == 0 || !compatible;
	display_requested = p_display || p_exact || !compatible;
	frame_time = p_time;
	interpolation_delay = compatible && !p_exact ? p_interval : 0;
	if (p_exact || !compatible || changed || frame_time < sample_times[history_index]) {
		history_count = 0;
	}
	evaluated = false;
	return !sample_requested || _capture(ObjectDB::get_instance<Skeleton3D>(skeleton_id), true);
}

void SkeletonAnimationPose::_update_globals() {
	GodotProfileZone("CZAnimation.Matrix");
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	lod.expand_global(blended, globals);
	const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;
	matrix_us += elapsed;
	matrix_us_total += elapsed;
}

Transform3D SkeletonAnimationPose::get_local_pose(int p_bone) const {
	CZAnimationBridge::complete_pose(this);
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Transform3D());
	const int compact = lod.to_compact(p_bone);
	return compact >= 0 ? _current_pose().get_transform(compact) : bones[p_bone].rest;
}

Transform3D SkeletonAnimationPose::get_global_pose(int p_bone) {
	CZAnimationBridge::complete_pose(this);
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Transform3D());
	return globals[p_bone];
}

Transform3D SkeletonAnimationPose::get_base_local_pose(int p_bone) const {
	CZAnimationBridge::complete_pose(this);
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Transform3D());
	const int compact = lod.to_compact(p_bone);
	return compact >= 0 ? (display_valid && !evaluating_pose ? displayed_base : base_pose).get_transform(compact) : bones[p_bone].rest;
}

Transform3D SkeletonAnimationPose::get_base_global_pose(int p_bone) const {
	CZAnimationBridge::complete_pose(this);
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Transform3D());
	return base_globals.is_empty() ? globals[p_bone] : base_globals[p_bone];
}

void SkeletonAnimationPose::set_bone_components(int p_bone, const Vector3 &p_position, const Quaternion &p_rotation, const Vector3 &p_scale, int p_mask) {
	ERR_FAIL_COND(!Thread::is_main_thread() && !evaluating_pose);
	Ref<SkeletonAnimationPose> keep_alive;
	if (Thread::is_main_thread()) {
		keep_alive = Ref<SkeletonAnimationPose>(this);
	}
	CZAnimationBridge::synchronize();
	if (Thread::is_main_thread() && !evaluating_pose) {
		capture_generation++;
	}
	if (!evaluating_pose && display_valid) {
		blended.copy_from(displayed);
		display_valid = false;
	}
	ERR_FAIL_INDEX(p_bone, int(bones.size()));
	const int compact = lod.to_compact(p_bone);
	if (compact < 0) {
		return;
	}
	if (p_mask & 1) {
		blended.set_position(compact, p_position);
	}
	if (p_mask & 2) {
		blended.set_rotation(compact, p_rotation);
	}
	if (p_mask & 4) {
		blended.set_scale(compact, p_scale);
	}
	_update_globals();
}

Quaternion SkeletonAnimationPose::get_bone_pose_rotation(int p_bone) const {
	CZAnimationBridge::complete_pose(this);
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Quaternion());
	const int compact = lod.to_compact(p_bone);
	return compact >= 0 ? _current_pose().get_rotation(compact) : bones[p_bone].rest.basis.get_rotation_quaternion();
}

Vector3 SkeletonAnimationPose::get_bone_pose_position(int p_bone) const {
	CZAnimationBridge::complete_pose(this);
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Vector3());
	const int compact = lod.to_compact(p_bone);
	return compact >= 0 ? _current_pose().get_position(compact) : bones[p_bone].rest.origin;
}

Vector3 SkeletonAnimationPose::get_bone_pose_scale(int p_bone) const {
	CZAnimationBridge::complete_pose(this);
	ERR_FAIL_INDEX_V(p_bone, int(bones.size()), Vector3(1, 1, 1));
	const int compact = lod.to_compact(p_bone);
	return compact >= 0 ? _current_pose().get_scale(compact) : bones[p_bone].rest.basis.get_scale();
}

void SkeletonAnimationPose::set_bone_pose_position(int p_bone, const Vector3 &p_position) {
	set_bone_components(p_bone, p_position, Quaternion(), Vector3(1, 1, 1), 1);
}

void SkeletonAnimationPose::set_bone_pose_scale(int p_bone, const Vector3 &p_scale) {
	set_bone_components(p_bone, Vector3(), Quaternion(), p_scale, 4);
}

void SkeletonAnimationPose::set_bone_pose_rotation(int p_bone, const Quaternion &p_rotation) {
	set_bone_components(p_bone, Vector3(), p_rotation, Vector3(1, 1, 1), 2);
}

void SkeletonAnimationPose::set_bone_global_pose(int p_bone, const Transform3D &p_transform) {
	ERR_FAIL_INDEX(p_bone, int(bones.size()));
	const int parent = bones[p_bone].parent;
	const Transform3D local = parent >= 0 ? globals[parent].affine_inverse() * p_transform : p_transform;
	set_bone_components(p_bone, local.origin, local.basis.get_rotation_quaternion(), local.basis.get_scale(), 7);
}

void SkeletonAnimationPose::evaluate() {
	Ref<SkeletonAnimationPose> keep_alive;
	if (Thread::is_main_thread()) {
		keep_alive = Ref<SkeletonAnimationPose>(this);
	}
	if (Thread::is_main_thread() && !CZAnimationBridge::is_processing_pose(this)) {
		CZAnimationBridge::synchronize();
	}
	if (!fallback_reason.is_empty()) {
		return;
	}
	evaluating_pose = true;
	display_valid = false;
	_update_globals();
	if (copy_source.is_valid()) {
		const Transform3D source_rest = copy_source->world * copy_source->bones[copy_source_bone].global_rest;
		const Transform3D target_rest = world * bones[copy_target_bone].global_rest;
		const Transform3D source_pose = copy_source->world * copy_source->get_global_pose(copy_source_bone);
		set_bone_global_pose(copy_target_bone, world.affine_inverse() * source_pose * source_rest.affine_inverse() * target_rest);
	}
	base_globals = globals;
	base_pose.copy_from(blended);
	generated_targets.clear();
	pawn_pose.reset_frame();
	pawn_pose.generate_targets();
	for (Modifier &modifier : modifiers) {
		const bool partial = modifier.influence < 1 && (modifier.aim || modifier.look_at || modifier.copy || modifier.two_bone);
		if (partial) {
			before_modifier.copy_from(blended);
		}
		if (modifier.aim) {
			pawn_pose.solve_aim();
		} else if (modifier.two_bone) {
			for (TwoBoneInput &input : modifier.two_bones) {
				_solve_two_bone(input);
			}
		} else if (modifier.copy) {
			for (CopyInput &copy : modifier.copies) {
				_solve_copy(copy);
			}
		} else if (modifier.look_at) {
			const int bone = modifier.look_bone;
			Transform3D pose_world = world_interpolated;
			if (attachment_source.is_valid() && attachment_bone >= 0 && attachment_bone < attachment_source->get_bone_count()) {
				pose_world = attachment_source->world_interpolated * attachment_source->get_global_pose(attachment_bone) * attachment_scale;
				if (attachment_disable_scale) {
					pose_world.basis.orthonormalize();
				}
				pose_world *= attachment_offset;
			}
			const Transform3D rest = modifier.look_solver.relative ? get_local_pose(bone) : bones[bone].rest;
			const int parent = bones[bone].parent;
			Transform3D bone_space = parent >= 0 ? pose_world * globals[parent] : pose_world;
			bone_space.translate_local(rest.origin);
			Transform3D origin = bone_space;
			if (modifier.origin_from == LookAtModifier3D::ORIGIN_FROM_SPECIFIC_BONE && modifier.origin_bone >= 0 && modifier.origin_bone < get_bone_count()) {
				origin = pose_world * globals[modifier.origin_bone];
			} else if (modifier.origin_from == LookAtModifier3D::ORIGIN_FROM_EXTERNAL_NODE && modifier.has_external_origin) {
				origin = modifier.external_origin;
			}
			const Quaternion result = modifier.look_solver.solve(bones[bone].rest, get_local_pose(bone), get_bone_pose_rotation(bone),
					bone_space, origin.translated_local(modifier.origin_offset), modifier.has_target, modifier.target);
			set_bone_pose_rotation(bone, result);
		} else {
			if (modifier.initial_global) {
				initial_global_rotations.resize(modifier.targets.size());
				for (uint32_t i = 0; i < modifier.targets.size(); i++) {
					initial_global_rotations[i] = globals[modifier.targets[i]].basis.get_rotation_quaternion();
				}
			}
			for (uint32_t i = 0; i < modifier.targets.size(); i++) {
				const real_t weight = CLAMP(modifier.weights[i] * modifier.influence, real_t(0), real_t(1));
				if (modifier.initial_global) {
					BoneRotationOffsetModifier3D::apply_from_initial_global_pose(this, modifier.targets[i], modifier.rotations[i], weight, initial_global_rotations[i]);
				} else {
					BoneRotationOffsetModifier3D::apply(this, modifier.targets[i], modifier.rotations[i], modifier.space, weight);
				}
			}
			pawn_pose.regenerate_grip_target();
		}
		if (partial) {
			for (int compact = 0; compact < blended.size(); compact++) {
				const Transform3D before = before_modifier.get_transform(compact);
				const Transform3D after = blended.get_transform(compact);
				if (before != after) {
					const Transform3D value = before.interpolate_with(after, modifier.influence);
					blended.set_position(compact, value.origin);
					blended.set_rotation(compact, value.basis.get_rotation_quaternion());
					blended.set_scale(compact, value.basis.get_scale());
				}
			}
			_update_globals();
		}
	}
	history_index ^= 1;
	history[history_index].copy_from(blended);
	base_history[history_index].copy_from(base_pose);
	sample_times[history_index] = frame_time;
	history_count = MIN(2, history_count + 1);
	evaluated = true;
	interpolate_frame();
	evaluating_pose = false;
}

void SkeletonAnimationPose::interpolate_frame() {
	if (!display_requested || history_count == 0) {
		return;
	}
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	{
		GodotProfileZone("CZAnimation.Interpolate");
		real_t weight = 1;
		const int previous = history_index ^ 1;
		const double span = sample_times[history_index] - sample_times[previous];
		if (history_count == 2 && interpolation_delay > 0 && span > 0 && span <= interpolation_delay + 0.000001) {
			weight = CLAMP((frame_time - interpolation_delay - sample_times[previous]) / span, 0.0, 1.0);
		}
		if (weight < 1) {
			displayed.interpolate(history[previous], history[history_index], weight, simd_enabled);
			displayed_base.interpolate(base_history[previous], base_history[history_index], weight, simd_enabled);
		} else {
			displayed.copy_from(history[history_index]);
			displayed_base.copy_from(base_history[history_index]);
		}
	}
	const uint64_t interpolate_elapsed = OS::get_singleton()->get_ticks_usec() - start;
	interpolate_us += interpolate_elapsed;
	interpolate_us_total += interpolate_elapsed;
	const uint64_t matrix_start = OS::get_singleton()->get_ticks_usec();
	{
		GodotProfileZone("CZAnimation.Matrix");
		lod.expand_global(displayed, globals);
		lod.expand_global(displayed_base, base_globals);
	}
	const uint64_t matrix_elapsed = OS::get_singleton()->get_ticks_usec() - matrix_start;
	matrix_us += matrix_elapsed;
	matrix_us_total += matrix_elapsed;
	_pack_skins();
	evaluate_sockets();
	evaluated = true;
	display_valid = true;
}

void SkeletonAnimationPose::_pack_skins() {
	GodotProfileZone("CZAnimation.SkinBufferPack");
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	for (SkinOutput &skin : skins) {
		const float *previous = skin.buffer.ptr();
		float *buffer = skin.buffer.ptrw();
		if (previous && previous != buffer) {
			skin_buffer_copies++;
		}
		for (uint32_t bind = 0; bind < skin.bones.size(); bind++) {
			const int bone = skin.bones[bind];
			Transform3D bind_pose = skin.bind_poses[bind];
			if (!bones[bone].skin_scale.is_equal_approx(Vector3(1, 1, 1))) {
				bind_pose = bind_pose.scaled(bones[bone].skin_scale);
			}
			const Transform3D value = globals[bone] * bind_pose;
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++) {
					buffer[bind * 12 + row * 4 + column] = value.basis.rows[row][column];
				}
				buffer[bind * 12 + row * 4 + 3] = value.origin[row];
			}
		}
	}
	const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;
	skin_pack_us += elapsed;
	skin_pack_us_total += elapsed;
}

uint64_t SkeletonAnimationPose::get_storage_bytes() const {
	uint64_t bytes = sizeof(SkeletonAnimationPose);
	bytes += lod.get_storage_bytes();
	bytes += sampling_scratch.get_storage_bytes();
	for (const CZPoseSoA *pose : { &reference_pose, &sampled, &blended, &displayed, &base_pose, &displayed_base, &retained_base, &before_modifier, &history[0], &history[1], &base_history[0], &base_history[1] }) {
		bytes += pose->get_storage_bytes();
	}
	bytes += uint64_t(bones.get_capacity()) * sizeof(Bone);
	bytes += uint64_t(globals.get_capacity() + base_globals.get_capacity() + rest_transforms.get_capacity()) * sizeof(Transform3D);
	bytes += uint64_t(parents.get_capacity() + first_disabled.get_capacity() + required.get_capacity() + required_next.get_capacity() + configured_required.get_capacity()) * sizeof(int);
	bytes += uint64_t(reference_mask.get_capacity()) * sizeof(uint8_t);
	bytes += uint64_t(skins.get_capacity()) * sizeof(SkinOutput);
	for (const SkinOutput &skin : skins) {
		bytes += uint64_t(skin.bones.get_capacity()) * sizeof(int);
		bytes += uint64_t(skin.bind_poses.get_capacity()) * sizeof(Transform3D);
		bytes += uint64_t(skin.buffer.capacity()) * sizeof(float);
	}
	return bytes;
}

uint64_t SkeletonAnimationPose::get_pose_copy_bytes() const {
	uint64_t bytes = 0;
	for (const CZPoseSoA *pose : { &reference_pose, &sampled, &blended, &displayed, &base_pose, &displayed_base, &retained_base, &before_modifier, &history[0], &history[1], &base_history[0], &base_history[1] }) {
		bytes += pose->get_copy_bytes();
	}
	return bytes;
}

uint64_t SkeletonAnimationPose::get_skin_buffer_copies() const {
	CZAnimationBridge::complete_pose(this);
	return skin_buffer_copies;
}

bool SkeletonAnimationPose::publish() {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	Ref<SkeletonAnimationPose> keep_alive(this);
	if (!CZAnimationBridge::is_processing_pose(this)) {
		CZAnimationBridge::synchronize();
	}
	const bool success = evaluated && CZAnimationBridge::publish_skeleton(this);
	evaluated = false;
	return success;
}

bool CZAnimationBridge::publish_skeleton(SkeletonAnimationPose *p_pose) {
	Skeleton3D *skeleton = ObjectDB::get_instance<Skeleton3D>(p_pose->skeleton_id);
	if (!skeleton || !p_pose->matches_current_inputs()) {
		return false;
	}
	const ObjectID id = skeleton->get_instance_id();
	const uint64_t generation = p_pose->capture_generation;
	const auto valid_capture = [&]() {
		return ObjectDB::get_instance(id) == skeleton && p_pose->capture_generation == generation &&
				p_pose->skeleton_id == id && p_pose->matches_current_inputs();
	};
	auto write_pose = [&](bool p_base) {
		const CZPoseSoA &local_pose = p_base ? p_pose->displayed_base : p_pose->displayed;
		const LocalVector<Transform3D> &global_pose = p_base ? p_pose->base_globals : p_pose->globals;
		for (uint32_t bone = 0; bone < p_pose->bones.size(); bone++) {
			Skeleton3D::Bone &target = skeleton->bones[bone];
			const int compact = p_pose->lod.to_compact(bone);
			const Transform3D local = compact >= 0 ? local_pose.get_transform(compact) : p_pose->bones[bone].rest;
			target.pose_position = compact >= 0 ? local_pose.get_position(compact) : local.origin;
			target.pose_rotation = compact >= 0 ? local_pose.get_rotation(compact) : local.basis.get_rotation_quaternion();
			target.pose_scale = compact >= 0 ? local_pose.get_scale(compact) : local.basis.get_scale();
			target.pose_cache = local;
			target.pose_cache_dirty = false;
			target.global_pose = global_pose[bone];
#ifndef DISABLE_DEPRECATED
			target.pose_global_no_override = global_pose[bone];
#endif
			skeleton->bone_global_pose_dirty[target.nested_set_offset] = false;
		}
		skeleton->dirty = false;
	};
	for (uint32_t index = 0; index < p_pose->generated_targets.size(); index++) {
		const SkeletonAnimationPose::GeneratedTarget generated = p_pose->generated_targets[index];
		if (Node3D *target = ObjectDB::get_instance<Node3D>(generated.id)) {
			if (generated.position_only) {
				target->set_position(generated.transform.origin);
			} else {
				target->set_transform(generated.transform);
			}
		}
		if (!valid_capture()) {
			return false;
		}
	}
	p_pose->pawn_pose.publish_targets();
	if (!valid_capture()) {
		return false;
	}
	skeleton->updating = true;
	write_pose(true);
	if (skeleton->has_connections(SNAME("pose_updated"))) {
		skeleton->emit_signal(SNAME("pose_updated"));
	}
	if (!valid_capture()) {
		if (ObjectDB::get_instance(id) == skeleton) {
			skeleton->updating = false;
		}
		return false;
	}
	write_pose(false);
	skeleton->emit_signal(SNAME("skeleton_updated"));
	if (!valid_capture()) {
		if (ObjectDB::get_instance(id) == skeleton) {
			skeleton->updating = false;
		}
		return false;
	}
	for (const SkeletonAnimationPose::SkinOutput &skin : p_pose->skins) {
		AnimationBatchProcessor::set_skeleton_buffer(skin.rid, skin.buffer);
	}
	// Signals, native attachments and skin palettes observe the final modifier
	// result. The next animation frame starts from its unmodified base pose.
	write_pose(true);
	skeleton->updating = false;
	skeleton->update_flags = Skeleton3D::UPDATE_FLAG_NONE;
	p_pose->retained_base.copy_from(p_pose->displayed_base);
	for (uint32_t index = 0; index < p_pose->modifiers.size(); index++) {
		const SkeletonAnimationPose::Modifier &modifier = p_pose->modifiers[index];
		if (modifier.two_bone) {
			if (TwoBoneIK3D *ik = ObjectDB::get_instance<TwoBoneIK3D>(modifier.id)) {
				for (const SkeletonAnimationPose::TwoBoneInput &input : modifier.two_bones) {
					if (!input.mutable_axes && input.setting_index < int(ik->tb_settings.size())) {
						auto *native = ik->tb_settings[input.setting_index];
						if (native->is_valid()) {
							native->root_pos = input.settings.root_pos;
							native->mid_pos = input.settings.mid_pos;
							native->end_pos = input.settings.end_pos;
							*native->root_joint_solver_info = input.root;
							*native->mid_joint_solver_info = input.middle;
						}
					}
				}
			}
		}
		if (modifier.look_at) {
			if (LookAtModifier3D *look_at = ObjectDB::get_instance<LookAtModifier3D>(modifier.id)) {
				publish_look_at(look_at, modifier.look_solver);
			}
		}
		if (Object *object = ObjectDB::get_instance(modifier.id)) {
			if (object->has_connections(SNAME("modification_processed"))) {
				object->emit_signal(SNAME("modification_processed"));
			}
		}
		if (!valid_capture()) {
			return false;
		}
	}
	return true;
}

void SkeletonAnimationPose::release() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	capture_generation++;
	if (!CZAnimationBridge::is_processing_pose(this)) {
		CZAnimationBridge::synchronize();
	}
	_release_capture();
}

void SkeletonAnimationPose::_release_capture() {
	history_count = 0;
	evaluated = false;
	display_valid = false;
	binding_dirty = true;
	if (owns_callback) {
		CZAnimationBridge::release_skeleton_control(skeleton_id);
		owns_callback = false;
	}
	if (observed) {
		CZAnimationBridge::unobserve_skeleton(skeleton_id);
		observed = false;
	}
}

void SkeletonAnimationPose::configure_copy_source(const Ref<SkeletonAnimationPose> &p_source, int p_source_bone, int p_target_bone) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	if (copy_source == p_source && copy_source_bone == p_source_bone && copy_target_bone == p_target_bone) {
		return;
	}
	Ref<SkeletonAnimationPose> keep_alive(this);
	const Ref<SkeletonAnimationPose> source = p_source;
	ERR_FAIL_COND(source.is_valid() && source->_depends_on(this));
	CZAnimationBridge::synchronize();
	capture_generation++;
	ERR_FAIL_COND(source.is_valid() && source->_depends_on(this));
	if (copy_source.is_valid()) {
		copy_source->_require_external(copy_source_bone, -1);
	}
	copy_source = source;
	copy_source_bone = p_source_bone;
	copy_target_bone = p_target_bone;
	if (source.is_valid() && p_source_bone >= 0) {
		source->_require_external(p_source_bone, 1);
	}
	binding_dirty = true;
}

void SkeletonAnimationPose::configure_attachment(const Ref<SkeletonAnimationPose> &p_source, int p_bone, const Transform3D &p_scale_transform, const Transform3D &p_offset, bool p_disable_scale) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	if (attachment_source == p_source && attachment_bone == p_bone && attachment_scale == p_scale_transform &&
			attachment_offset == p_offset && attachment_disable_scale == p_disable_scale) {
		return;
	}
	Ref<SkeletonAnimationPose> keep_alive(this);
	const Ref<SkeletonAnimationPose> source = p_source;
	ERR_FAIL_COND(source.is_valid() && source->_depends_on(this));
	CZAnimationBridge::synchronize();
	capture_generation++;
	ERR_FAIL_COND(source.is_valid() && source->_depends_on(this));
	const bool dependency_changed = source != attachment_source || p_bone != attachment_bone;
	if (!dependency_changed) {
		attachment_same_source_updates_total++;
	} else {
		attachment_dependency_changes_total++;
	}
	// Updating attachment matrices does not change the parent's required bones.
	if (dependency_changed && attachment_source.is_valid()) {
		attachment_source->_require_external(attachment_bone, -1);
	}
	attachment_source = source;
	attachment_bone = p_bone;
	attachment_scale = p_scale_transform;
	attachment_offset = p_offset;
	attachment_disable_scale = p_disable_scale;
	if (dependency_changed && source.is_valid() && p_bone >= 0) {
		source->_require_external(p_bone, 1);
	}
	if (dependency_changed) {
		binding_dirty = true;
	}
}

void SkeletonAnimationPose::set_socket_input(int p_index, int p_bone, const Vector3 &p_offset, const Transform3D &p_parent, const Basis &p_marker_basis, bool p_enabled) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	ERR_FAIL_COND(p_index < 0);
	CZAnimationBridge::synchronize();
	capture_generation++;
	if (int(sockets.size()) <= p_index) {
		sockets.resize(p_index + 1);
	}
	if (sockets[p_index].bone != p_bone || sockets[p_index].enabled != p_enabled) {
		binding_dirty = true;
	}
	sockets[p_index] = { p_bone, p_offset, p_parent, p_marker_basis, Transform3D(), p_enabled };
}

Transform3D SkeletonAnimationPose::get_socket_transform(int p_index) const {
	CZAnimationBridge::complete_pose(this);
	ERR_FAIL_INDEX_V(p_index, int(sockets.size()), Transform3D());
	return sockets[p_index].result;
}

void SkeletonAnimationPose::evaluate_sockets() {
	Transform3D socket_world = world;
	if (attachment_source.is_valid() && attachment_bone >= 0 && attachment_bone < attachment_source->get_bone_count()) {
		socket_world = attachment_source->world * attachment_source->get_global_pose(attachment_bone) * attachment_scale;
		if (attachment_disable_scale) {
			socket_world.basis.orthonormalize();
		}
		socket_world *= attachment_offset;
	}
	for (Socket &socket : sockets) {
		if (!socket.enabled) {
			continue;
		}
		if (socket.bone < 0) {
			socket.result = Transform3D(socket.marker, socket.offset);
		} else if (socket.bone < get_bone_count()) {
			const Transform3D bone_world = socket_world * get_global_pose(socket.bone);
			const Transform3D parent_world = socket_world * socket.parent;
			socket.result = parent_world.affine_inverse() * Transform3D(bone_world.basis.orthonormalized(), bone_world.xform(socket.offset));
		}
	}
}

void SkeletonAnimationPose::set_target(ObjectID p_id, const Transform3D &p_target, bool p_position_only) {
	if (!p_id.is_valid()) {
		return;
	}
	for (GeneratedTarget &target : generated_targets) {
		if (target.id == p_id) {
			target.transform = p_target;
			target.position_only = p_position_only;
			return;
		}
	}
	generated_targets.push_back({ p_id, p_target, p_position_only });
}

Transform3D SkeletonAnimationPose::_chain_rest(int p_bone, int p_root, bool p_mutable) {
	if (!p_mutable) {
		return bones[p_bone].global_rest;
	}
	Transform3D tr = bones[p_root].global_rest;
	tr.origin = tr.origin - bones[p_root].rest.origin + get_local_pose(p_root).origin;
	scratch_path.clear();
	for (int bone = p_bone; bone != p_root && bone != -1; bone = bones[bone].parent) {
		scratch_path.push_back(bone);
	}
	for (int i = int(scratch_path.size()) - 1; i >= 0; i--) {
		const auto &b = bones[scratch_path[i]];
		tr = tr * Transform3D(b.rest.basis, get_local_pose(scratch_path[i]).origin);
	}
	return tr;
}

void SkeletonAnimationPose::_solve_two_bone(TwoBoneInput &r_input) {
	for (const GeneratedTarget &generated : generated_targets) {
		if (generated.id == r_input.target_id) {
			r_input.target = generated.transform.origin;
		}
		if (generated.id == r_input.pole_id) {
			r_input.pole = generated.transform.origin;
		}
	}
	auto &s = r_input.settings;
	s.root_joint_solver_info = &r_input.root;
	s.mid_joint_solver_info = &r_input.middle;
	if (!r_input.mutable_axes) {
		CZModifierSolvers::cache_two_bone_rotations(this, &s, r_input.pole);
		CZModifierSolvers::solve_two_bone(this, &s, r_input.target, r_input.pole);
		return;
	}
	int end = s.get_end_bone();
	Vector3 axis;
	if (s.end_bone_direction == SkeletonModifier3D::BONE_DIRECTION_FROM_PARENT) {
		axis = bones[end].rest.basis.xform_inv(r_input.mutable_axes ? get_local_pose(end).origin : bones[end].rest.origin).normalized();
	} else {
		axis = SkeletonModifier3D::get_vector_from_bone_axis(static_cast<SkeletonModifier3D::BoneAxis>(int(s.end_bone_direction)));
	}
	Vector3 global_rest_origin;
	if (s.extend_end_bone && s.end_bone_length > 0 && !axis.is_zero_approx()) {
		s.end_pos = get_global_pose(end).xform(axis * s.end_bone_length);
		global_rest_origin = _chain_rest(end, s.root_bone.bone, r_input.mutable_axes).xform(axis * s.end_bone_length);
	} else {
		s.end_pos = get_global_pose(end).origin;
		global_rest_origin = _chain_rest(end, s.root_bone.bone, r_input.mutable_axes).origin;
	}
	Vector3 origin = _chain_rest(s.middle_bone.bone, s.root_bone.bone, r_input.mutable_axes).origin;
	axis = global_rest_origin - origin;
	global_rest_origin = origin;
	if (axis.is_zero_approx()) {
		return;
	}
	s.mid_pos = get_global_pose(s.middle_bone.bone).origin;
	r_input.middle.forward_vector = bones[s.middle_bone.bone].global_rest.basis.get_rotation_quaternion().xform_inv(axis).normalized();
	r_input.middle.length = axis.length();
	origin = _chain_rest(s.root_bone.bone, s.root_bone.bone, r_input.mutable_axes).origin;
	axis = global_rest_origin - origin;
	if (axis.is_zero_approx()) {
		return;
	}
	s.root_pos = get_global_pose(s.root_bone.bone).origin;
	r_input.root.forward_vector = bones[s.root_bone.bone].global_rest.basis.get_rotation_quaternion().xform_inv(axis).normalized();
	r_input.root.length = axis.length();
	CZModifierSolvers::init_two_bone_rotations(this, &s);
	double length = r_input.root.length + r_input.middle.length;
	s.cached_length_sq = length * length;
	CZModifierSolvers::cache_two_bone_rotations(this, &s, r_input.pole);
	CZModifierSolvers::solve_two_bone(this, &s, r_input.target, r_input.pole);
}

void SkeletonAnimationPose::_solve_copy(CopyInput &r_input) {
	auto &settings = r_input.settings;
	for (const GeneratedTarget &generated : generated_targets) {
		if (generated.id == r_input.target_id) {
			if (generated.position_only) {
				r_input.target.origin = generated.transform.origin;
			} else {
				r_input.target = generated.transform;
			}
		}
	}
	Transform3D destination;
	Transform3D to_apply_space;
	if (settings.reference_type == BoneConstraint3D::REFERENCE_TYPE_NODE) {
		const int parent = bones[settings.apply_bone].parent;
		destination = parent >= 0 ? globals[parent].affine_inverse() * r_input.target : r_input.target;
	} else {
		if (settings.is_global()) {
			const int parent = bones[settings.apply_bone].parent;
			to_apply_space = parent >= 0 ? globals[parent].affine_inverse() : Transform3D();
			destination = to_apply_space * globals[settings.reference_bone];
		} else {
			destination = get_local_pose(settings.reference_bone);
		}
		if (settings.is_relative()) {
			const Transform3D rest = settings.is_global() ? to_apply_space * bones[settings.reference_bone].global_rest : bones[settings.reference_bone].rest;
			const Vector3 scale = destination.basis.get_scale() / rest.basis.get_scale();
			destination.basis = rest.basis.get_rotation_quaternion().inverse() * destination.basis.get_rotation_quaternion();
			destination.basis.scale_local(scale);
			destination.origin -= rest.origin;
		}
	}
	CZModifierSolvers::copy_pose(this, &settings, settings.apply_bone, destination, settings.amount);
}

void SkeletonAnimationPose::configure_aim(uint64_t p_modifier_id, const Dictionary &p_input) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	CZAnimationBridge::synchronize();
	capture_generation++;
	aim_configuration = p_input.duplicate();
	pawn_pose.configure_aim(p_modifier_id, p_input);
	_refresh_configured_requirements();
}

void SkeletonAnimationPose::configure_ik(const Dictionary &p_input) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	CZAnimationBridge::synchronize();
	capture_generation++;
	ik_configuration = p_input.duplicate();
	pawn_pose.configure_ik(p_input);
	_refresh_configured_requirements();
}

void SkeletonAnimationPose::_refresh_configured_requirements() {
	configured_required.clear();
	for (const Dictionary &input : { aim_configuration, ik_configuration }) {
		for (const Variant &key : input.keys()) {
			if (String(key).ends_with("_bone")) {
				const int bone = input[key];
				if (bone >= 0 && !configured_required.has(bone)) {
					configured_required.push_back(bone);
				}
			}
		}
	}
	binding_dirty = true;
}

void SkeletonAnimationPose::_require_external(int p_bone, int p_delta) {
	if (p_bone < 0) {
		return;
	}
	external_requirement_updates_total++;
	int *count = external_requirements.getptr(p_bone);
	if (p_delta > 0) {
		external_requirements[p_bone] = count ? *count + p_delta : p_delta;
	} else if (count) {
		*count += p_delta;
		if (*count <= 0) {
			external_requirements.erase(p_bone);
		}
	}
	binding_dirty = true;
}

bool SkeletonAnimationPose::_depends_on(const SkeletonAnimationPose *p_pose) const {
	LocalVector<const SkeletonAnimationPose *> pending = { this };
	LocalVector<const SkeletonAnimationPose *> visited;
	while (!pending.is_empty()) {
		const SkeletonAnimationPose *pose = pending[pending.size() - 1];
		pending.remove_at(pending.size() - 1);
		if (pose == p_pose) {
			return true;
		}
		if (visited.has(pose)) {
			continue;
		}
		visited.push_back(pose);
		if (pose->copy_source.is_valid()) {
			pending.push_back(pose->copy_source.ptr());
		}
		if (pose->attachment_source.is_valid()) {
			pending.push_back(pose->attachment_source.ptr());
		}
	}
	return false;
}

void SkeletonAnimationPose::set_aim_input(real_t p_hip_weight, const Transform3D &p_muzzle, bool p_has_grip, const Transform3D &p_grip, const Vector3 &p_target, const Vector3 &p_up) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	CZAnimationBridge::synchronize();
	capture_generation++;
	pawn_pose.set_aim_input(p_hip_weight, p_muzzle, p_has_grip, p_grip, p_target, p_up);
}
void SkeletonAnimationPose::set_ik_hand_input(bool p_has_target, const Transform3D &p_target, const Vector3 &p_weights) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	CZAnimationBridge::synchronize();
	capture_generation++;
	pawn_pose.set_ik_hand_input(p_has_target, p_target, p_weights);
}
void SkeletonAnimationPose::set_ik_ground_input(bool p_left_hit, const Vector3 &p_left_position, const Vector3 &p_left_normal, bool p_right_hit, const Vector3 &p_right_position, const Vector3 &p_right_normal) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	CZAnimationBridge::synchronize();
	capture_generation++;
	pawn_pose.set_ik_ground_input(p_left_hit, p_left_position, p_left_normal, p_right_hit, p_right_position, p_right_normal);
}
void SkeletonAnimationPose::set_ik_ground_motion_input(bool p_left_hit, const Vector3 &p_left_position, const Vector3 &p_left_normal, const Vector3 &p_left_smoothed_normal, bool p_right_hit, const Vector3 &p_right_position, const Vector3 &p_right_normal, const Vector3 &p_right_smoothed_normal) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<SkeletonAnimationPose> keep_alive(this);
	CZAnimationBridge::synchronize();
	capture_generation++;
	pawn_pose.set_ik_ground_motion_input(p_left_hit, p_left_position, p_left_normal, p_left_smoothed_normal, p_right_hit, p_right_position, p_right_normal, p_right_smoothed_normal);
}
Transform3D SkeletonAnimationPose::get_aim_muzzle() const {
	CZAnimationBridge::complete_pose(this);
	return pawn_pose.get_aim_muzzle();
}
real_t SkeletonAnimationPose::get_aim_error_degrees() const {
	CZAnimationBridge::complete_pose(this);
	return pawn_pose.get_aim_error_degrees();
}

SkeletonAnimationPose::~SkeletonAnimationPose() {
	if (Thread::is_main_thread()) {
		release();
		if (copy_source.is_valid()) {
			copy_source->_require_external(copy_source_bone, -1);
		}
		if (attachment_source.is_valid()) {
			attachment_source->_require_external(attachment_bone, -1);
		}
	} else if (owns_callback || observed || copy_source.is_valid() || attachment_source.is_valid()) {
		Cleanup *payload = memnew(Cleanup);
		payload->skeleton = skeleton_id;
		payload->owns_callback = owns_callback;
		payload->observed = observed;
		SWAP(payload->copy_source, copy_source);
		payload->copy_source_bone = copy_source_bone;
		SWAP(payload->attachment_source, attachment_source);
		payload->attachment_bone = attachment_bone;
		MutexLock lock(cleanup_mutex);
		deferred_cleanup.push_back(payload);
	}
}

void SkeletonAnimationPose::drain_deferred_cleanup() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	LocalVector<Cleanup *> cleanup;
	{
		MutexLock lock(cleanup_mutex);
		SWAP(cleanup, deferred_cleanup);
	}
	for (Cleanup *payload : cleanup) {
		if (payload->owns_callback) {
			CZAnimationBridge::release_skeleton_control(payload->skeleton);
		}
		if (payload->observed) {
			CZAnimationBridge::unobserve_skeleton(payload->skeleton);
		}
		if (payload->copy_source.is_valid()) {
			payload->copy_source->_require_external(payload->copy_source_bone, -1);
		}
		if (payload->attachment_source.is_valid()) {
			payload->attachment_source->_require_external(payload->attachment_bone, -1);
		}
		memdelete(payload);
	}
}

void SkeletonAnimationPose::_bind_methods() {
	ClassDB::bind_method(D_METHOD("capture", "skeleton"), &SkeletonAnimationPose::capture);
	ClassDB::bind_method(D_METHOD("capture_current"), &SkeletonAnimationPose::capture_current);
	ClassDB::bind_method(D_METHOD("configure_bone_lod", "bone_names", "first_disabled_lod"), &SkeletonAnimationPose::configure_bone_lod);
	ClassDB::bind_method(D_METHOD("set_bone_lod", "level"), &SkeletonAnimationPose::set_bone_lod);
	ClassDB::bind_method(D_METHOD("get_bone_lod"), &SkeletonAnimationPose::get_bone_lod);
	ClassDB::bind_method(D_METHOD("get_active_bone_count"), &SkeletonAnimationPose::get_active_bone_count);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &SkeletonAnimationPose::get_bone_count);
	ClassDB::bind_method(D_METHOD("get_skin_buffer_copies"), &SkeletonAnimationPose::get_skin_buffer_copies);
	ClassDB::bind_method(D_METHOD("get_base_local_pose", "bone"), &SkeletonAnimationPose::get_base_local_pose);
	ClassDB::bind_method(D_METHOD("get_base_global_pose", "bone"), &SkeletonAnimationPose::get_base_global_pose);
	ClassDB::bind_method(D_METHOD("get_global_pose", "bone"), &SkeletonAnimationPose::get_global_pose);
	ClassDB::bind_method(D_METHOD("get_local_pose", "bone"), &SkeletonAnimationPose::get_local_pose);
	ClassDB::bind_method(D_METHOD("get_motion_scale"), &SkeletonAnimationPose::get_motion_scale);
	ClassDB::bind_method(D_METHOD("get_skeleton_instance_id"), &SkeletonAnimationPose::get_skeleton_instance_id);
	ClassDB::bind_method(D_METHOD("get_fallback_reason"), &SkeletonAnimationPose::get_fallback_reason);
	ClassDB::bind_method(D_METHOD("supports_interpolation"), &SkeletonAnimationPose::supports_interpolation);
	ClassDB::bind_method(D_METHOD("release"), &SkeletonAnimationPose::release);
	ClassDB::bind_method(D_METHOD("evaluate"), &SkeletonAnimationPose::evaluate);
	ClassDB::bind_method(D_METHOD("publish"), &SkeletonAnimationPose::publish);
	ClassDB::bind_method(D_METHOD("set_bone_components", "bone", "position", "rotation", "scale", "mask"), &SkeletonAnimationPose::set_bone_components);
	ClassDB::bind_method(D_METHOD("configure_copy_source", "source", "source_bone", "target_bone"), &SkeletonAnimationPose::configure_copy_source);
	ClassDB::bind_method(D_METHOD("configure_attachment", "source", "bone", "scale_transform", "offset", "disable_scale"), &SkeletonAnimationPose::configure_attachment);
	ClassDB::bind_method(D_METHOD("set_socket_input", "index", "bone", "offset", "parent", "marker_basis", "enabled"), &SkeletonAnimationPose::set_socket_input);
	ClassDB::bind_method(D_METHOD("get_socket_transform", "index"), &SkeletonAnimationPose::get_socket_transform);
	ClassDB::bind_method(D_METHOD("configure_aim", "modifier_id", "input"), &SkeletonAnimationPose::configure_aim);
	ClassDB::bind_method(D_METHOD("configure_ik", "input"), &SkeletonAnimationPose::configure_ik);
	ClassDB::bind_method(D_METHOD("set_aim_input", "hip_weight", "muzzle", "has_grip", "grip", "target", "up"), &SkeletonAnimationPose::set_aim_input);
	ClassDB::bind_method(D_METHOD("set_ik_hand_input", "has_target", "target", "weights"), &SkeletonAnimationPose::set_ik_hand_input);
	ClassDB::bind_method(D_METHOD("set_ik_ground_input", "left_hit", "left_position", "left_normal", "right_hit", "right_position", "right_normal"), &SkeletonAnimationPose::set_ik_ground_input);
	ClassDB::bind_method(D_METHOD("set_ik_ground_motion_input", "left_hit", "left_position", "left_normal", "left_smoothed_normal", "right_hit", "right_position", "right_normal", "right_smoothed_normal"), &SkeletonAnimationPose::set_ik_ground_motion_input);
	ClassDB::bind_method(D_METHOD("get_aim_muzzle"), &SkeletonAnimationPose::get_aim_muzzle);
	ClassDB::bind_method(D_METHOD("get_aim_error_degrees"), &SkeletonAnimationPose::get_aim_error_degrees);
}
