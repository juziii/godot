#include "transform_batch_processor.h"

#ifndef _3D_DISABLED
#include "core/object/class_db.h"
#include "scene/3d/physics/collision_object_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/physics_3d/physics_server_3d_enums.h"

void TransformBatchProcessor::sync_transforms(const PackedInt64Array &instance_ids, const TypedArray<Transform3D> &transforms) {
	const int64_t count = MIN(instance_ids.size(), (int64_t)transforms.size());
	for (int64_t i = 0; i < count; i++) {
		Node3D *node = Object::cast_to<Node3D>(ObjectDB::get_instance(ObjectID(static_cast<uint64_t>(instance_ids[i]))));
		if (node == nullptr || !node->is_inside_tree()) {
			skipped_count++;
			continue;
		}
		const Transform3D transform = transforms[i];
		node->set_global_transform(transform);
		// Physics bodies commit immediately so logic steps later in this frame
		// query the authoritative pose; the deferred notification push repeats
		// the same value and is idempotent.
		if (CollisionObject3D *collision = Object::cast_to<CollisionObject3D>(node)) {
			PhysicsServer3D::get_singleton()->body_set_state(collision->get_rid(), PhysicsServer3DEnums::BODY_STATE_TRANSFORM, transform);
		}
		committed_count++;
	}
}

int TransformBatchProcessor::get_committed_count() const {
	return committed_count;
}

int TransformBatchProcessor::get_skipped_count() const {
	return skipped_count;
}

void TransformBatchProcessor::reset_counters() {
	committed_count = 0;
	skipped_count = 0;
}

void TransformBatchProcessor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("sync_transforms", "instance_ids", "transforms"), &TransformBatchProcessor::sync_transforms);
	ClassDB::bind_method(D_METHOD("get_committed_count"), &TransformBatchProcessor::get_committed_count);
	ClassDB::bind_method(D_METHOD("get_skipped_count"), &TransformBatchProcessor::get_skipped_count);
	ClassDB::bind_method(D_METHOD("reset_counters"), &TransformBatchProcessor::reset_counters);
}
#endif // _3D_DISABLED
