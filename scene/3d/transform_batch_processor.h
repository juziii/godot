#pragma once

#ifndef _3D_DISABLED
#include "core/object/ref_counted.h"
#include "core/variant/typed_array.h"

// Batched simulation-to-scene transform commits for crowds. The managed side
// collects the actors whose authoritative transform changed during a tick and
// crosses the managed/native boundary once per tick instead of once per actor.
// Each entry writes Node3D::set_global_transform() (dirty marking, deferred
// notification/flush semantics unchanged) plus an immediate physics body state
// push so same-frame logic queries observe the authoritative pose while visual
// children and notifications still ride the engine's deferred transform flush.
// Main thread only; call from the same thread that owns the scene tree.
class TransformBatchProcessor : public RefCounted {
	GDCLASS(TransformBatchProcessor, RefCounted);

	int committed_count = 0;
	int skipped_count = 0;

protected:
	static void _bind_methods();

public:
	void sync_transforms(const PackedInt64Array &instance_ids, const TypedArray<Transform3D> &transforms);
	int get_committed_count() const;
	int get_skipped_count() const;
	void reset_counters();

	TransformBatchProcessor() = default;
};
#endif // _3D_DISABLED
