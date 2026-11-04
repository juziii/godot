/**************************************************************************/
/*  test_physical_bone_simulator.cpp                                      */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_physical_bone_simulator)

#if !defined(_3D_DISABLED) && !defined(PHYSICS_3D_DISABLED)

#include "scene/3d/physics/physical_bone_3d.h"
#include "scene/3d/physics/physical_bone_simulator_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

namespace TestPhysicalBoneSimulator {

static bool has_pose_subscription(Skeleton3D *p_skeleton, ObjectID p_simulator) {
	List<Object::Connection> connections;
	p_skeleton->get_signal_connection_list("pose_updated", &connections);
	for (const Object::Connection &connection : connections) {
		if (connection.callable.get_object_id() == p_simulator) {
			return true;
		}
	}
	return false;
}

struct Fixture {
	Node3D *container = memnew(Node3D);
	Skeleton3D *skeleton = nullptr;
	PhysicalBoneSimulator3D *simulator = nullptr;

	Fixture() {
		SceneTree::get_singleton()->get_root()->add_child(container);
		skeleton = make_skeleton(false);
		simulator = memnew(PhysicalBoneSimulator3D);
		skeleton->add_child(simulator);
	}

	Skeleton3D *make_skeleton(bool p_reverse) {
		Skeleton3D *result = memnew(Skeleton3D);
		const int first = result->add_bone(p_reverse ? "tip" : "root");
		const int second = result->add_bone(p_reverse ? "root" : "tip");
		result->set_bone_parent(p_reverse ? first : second, p_reverse ? second : first);
		result->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
		container->add_child(result);
		return result;
	}

	PhysicalBone3D *add_physical(const String &p_name) {
		PhysicalBone3D *physical = memnew(PhysicalBone3D);
		physical->set_bone_name(p_name);
		simulator->add_child(physical);
		return physical;
	}

	~Fixture() {
		container->get_parent()->remove_child(container);
		memdelete(container);
	}
};

TEST_CASE("[SceneTree][PhysicalBoneSimulator3D] Empty simulator reads owner pose without subscribing") {
	Fixture fixture;
	CHECK(fixture.simulator->get_bone_count() == 2);
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	fixture.skeleton->set_bone_pose_position(0, Vector3(2, 3, 4));
	fixture.skeleton->set_bone_pose_position(1, Vector3(0, 5, 0));
	CHECK(fixture.simulator->get_bone_global_pose(1).origin.is_equal_approx(Vector3(2, 8, 4)));
	fixture.skeleton->set_bone_pose_position(0, Vector3(7, 8, 9));
	CHECK(fixture.simulator->get_bone_global_pose(1).origin.is_equal_approx(Vector3(7, 13, 9)));
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	const int added = fixture.skeleton->add_bone("added");
	fixture.skeleton->set_bone_pose_position(added, Vector3(10, 11, 12));
	CHECK(fixture.simulator->get_bone_global_pose(added).origin.is_equal_approx(Vector3(10, 11, 12)));
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
}

TEST_CASE("[SceneTree][PhysicalBoneSimulator3D] First binding subscribes and last removal disconnects") {
	Fixture fixture;
	fixture.skeleton->set_bone_pose_position(0, Vector3(1, 2, 3));
	PhysicalBone3D *first = fixture.add_physical("root");
	PhysicalBone3D *last = fixture.add_physical("tip");
	CHECK(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	CHECK(fixture.simulator->get_physical_bone(0) == first);
	CHECK(fixture.simulator->get_physical_bone(1) == last);
	CHECK(fixture.simulator->get_bone_global_pose(0).origin.is_equal_approx(Vector3(1, 2, 3)));
	fixture.skeleton->set_bone_pose_position(0, Vector3(4, 5, 6));
	fixture.skeleton->emit_signal("pose_updated");
	CHECK(fixture.simulator->get_bone_global_pose(0).origin.is_equal_approx(Vector3(4, 5, 6)));
	fixture.simulator->remove_child(first);
	memdelete(first);
	CHECK(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	fixture.simulator->remove_child(last);
	memdelete(last);
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	fixture.skeleton->set_bone_pose_position(0, Vector3(8, 9, 10));
	CHECK(fixture.simulator->get_bone_global_pose(0).origin.is_equal_approx(Vector3(8, 9, 10)));
	const ObjectID id = fixture.simulator->get_instance_id();
	fixture.skeleton->remove_child(fixture.simulator);
	memdelete(fixture.simulator);
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, id));
}

TEST_CASE("[SceneTree][PhysicalBoneSimulator3D] Renaming releases old slots and missing names stop simulation") {
	Fixture fixture;
	PhysicalBone3D *physical = fixture.add_physical("root");
	physical->set_bone_name("tip");
	CHECK(physical->get_bone_id() == 1);
	CHECK(fixture.simulator->get_physical_bone(0) == nullptr);
	CHECK(fixture.simulator->get_physical_bone(1) == physical);
	CHECK(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	fixture.simulator->physical_bones_start_simulation_on(TypedArray<StringName>());
	CHECK(physical->is_simulating_physics());
	physical->set_bone_name("missing");
	CHECK(physical->get_bone_id() == -1);
	CHECK_FALSE(physical->is_simulating_physics());
	CHECK(fixture.simulator->get_physical_bone(0) == nullptr);
	CHECK(fixture.simulator->get_physical_bone(1) == nullptr);
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	physical->set_bone_name("root");
	CHECK(physical->get_bone_id() == 0);
	CHECK(physical->is_simulating_physics());
	CHECK(fixture.simulator->get_physical_bone(0) == physical);
	CHECK(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	fixture.simulator->remove_child(physical);
	physical->set_bone_name("tip"); // A detached node must not rebind its old owner.
	CHECK(physical->get_bone_id() == -1);
	CHECK(fixture.simulator->get_physical_bone(0) == nullptr);
	CHECK(fixture.simulator->get_physical_bone(1) == nullptr);
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	memdelete(physical);
}

TEST_CASE("[SceneTree][PhysicalBoneSimulator3D] A conflicting rename cannot unbind the existing body") {
	Fixture fixture;
	PhysicalBone3D *first = fixture.add_physical("root");
	PhysicalBone3D *existing = fixture.add_physical("tip");
	ERR_PRINT_OFF;
	first->set_bone_name("tip");
	ERR_PRINT_ON;
	CHECK(first->get_bone_id() == -1);
	CHECK(fixture.simulator->get_physical_bone(0) == nullptr);
	CHECK(fixture.simulator->get_physical_bone(1) == existing);
	fixture.simulator->remove_child(first);
	memdelete(first);
	CHECK(fixture.simulator->get_physical_bone(1) == existing);
	CHECK(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	fixture.simulator->remove_child(existing);
	memdelete(existing);
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
}

TEST_CASE("[SceneTree][PhysicalBoneSimulator3D] Additions and hierarchy edits preserve named bindings") {
	Fixture fixture;
	PhysicalBone3D *root = fixture.add_physical("root");
	PhysicalBone3D *tip = fixture.add_physical("tip");
	CHECK(fixture.simulator->get_physical_bone_parent(1) == root);
	const int added = fixture.skeleton->add_bone("added");
	fixture.skeleton->set_bone_parent(added, 1);
	fixture.skeleton->get_parentless_bones(); // Complete the owner's structural update.
	CHECK(fixture.simulator->get_bone_count() == 3);
	CHECK(fixture.simulator->get_physical_bone(0) == root);
	CHECK(fixture.simulator->get_physical_bone(1) == tip);
	CHECK(fixture.simulator->get_physical_bone_parent(added) == tip);
	fixture.skeleton->set_bone_parent(1, -1);
	fixture.skeleton->get_parentless_bones();
	CHECK(fixture.simulator->get_physical_bone(0) == root);
	CHECK(fixture.simulator->get_physical_bone(1) == tip);
	CHECK(fixture.simulator->get_physical_bone_parent(1) == nullptr);
	CHECK(fixture.simulator->get_physical_bone_parent(added) == tip);
	CHECK(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	const int immediately_bound = fixture.skeleton->add_bone("immediately_bound");
	PhysicalBone3D *new_physical = fixture.add_physical("immediately_bound");
	CHECK(new_physical->get_bone_id() == immediately_bound);
	CHECK(fixture.simulator->get_physical_bone(immediately_bound) == new_physical);
}

TEST_CASE("[SceneTree][PhysicalBoneSimulator3D] Reverse index hierarchy clears cached parents on unbind") {
	Fixture fixture;
	fixture.skeleton->clear_bones();
	fixture.skeleton->add_bone("tip");
	fixture.skeleton->add_bone("middle");
	fixture.skeleton->add_bone("root");
	fixture.skeleton->set_bone_parent(0, 1);
	fixture.skeleton->set_bone_parent(1, 2);
	fixture.skeleton->get_parentless_bones();
	PhysicalBone3D *root = fixture.add_physical("root");
	fixture.add_physical("tip");
	CHECK(fixture.simulator->get_physical_bone_parent(0) == root);
	fixture.simulator->remove_child(root);
	memdelete(root);
	CHECK(fixture.simulator->get_physical_bone_parent(0) == nullptr);
	CHECK(fixture.simulator->get_physical_bone_parent(1) == nullptr);
}

TEST_CASE("[SceneTree][PhysicalBoneSimulator3D] Clear and replacement lists never retain old index bindings") {
	Fixture fixture;
	PhysicalBone3D *root = fixture.add_physical("root");
	PhysicalBone3D *tip = fixture.add_physical("tip");
	fixture.skeleton->clear_bones();
	fixture.skeleton->get_parentless_bones();
	CHECK(fixture.simulator->get_bone_count() == 0);
	CHECK(root->get_bone_id() == -1);
	CHECK(tip->get_bone_id() == -1);
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
	fixture.skeleton->add_bone("replacement");
	fixture.skeleton->get_parentless_bones();
	CHECK(fixture.simulator->get_physical_bone(0) == nullptr);
	const int new_tip = fixture.skeleton->add_bone("tip");
	const int new_root = fixture.skeleton->add_bone("root");
	fixture.skeleton->set_bone_parent(new_tip, new_root);
	fixture.skeleton->get_parentless_bones();
	CHECK(root->get_bone_id() == new_root);
	CHECK(tip->get_bone_id() == new_tip);
	CHECK(fixture.simulator->get_physical_bone(new_root) == root);
	CHECK(fixture.simulator->get_physical_bone(new_tip) == tip);
	CHECK(fixture.simulator->get_physical_bone(0) == nullptr);
	CHECK(fixture.simulator->get_physical_bone_parent(new_tip) == root);
	CHECK(has_pose_subscription(fixture.skeleton, fixture.simulator->get_instance_id()));
}

TEST_CASE("[SceneTree][PhysicalBoneSimulator3D] Reparenting resolves the new skeleton and disconnects the old one") {
	Fixture fixture;
	PhysicalBone3D *root = fixture.add_physical("root");
	PhysicalBone3D *tip = fixture.add_physical("tip");
	Skeleton3D *other = fixture.make_skeleton(true);
	other->set_bone_pose_position(1, Vector3(3, 4, 5));
	const ObjectID id = fixture.simulator->get_instance_id();
	fixture.simulator->reparent(other, false);
	CHECK(fixture.simulator->get_skeleton() == other);
	CHECK_FALSE(has_pose_subscription(fixture.skeleton, id));
	CHECK(has_pose_subscription(other, id));
	CHECK(root->get_bone_id() == 1);
	CHECK(tip->get_bone_id() == 0);
	CHECK(fixture.simulator->get_physical_bone(1) == root);
	CHECK(fixture.simulator->get_physical_bone(0) == tip);
	CHECK(fixture.simulator->get_physical_bone_parent(0) == root);
	CHECK(fixture.simulator->get_bone_global_pose(1).origin.is_equal_approx(Vector3(3, 4, 5)));
}

} // namespace TestPhysicalBoneSimulator

#endif // !_3D_DISABLED && !PHYSICS_3D_DISABLED
