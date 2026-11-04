/**************************************************************************/
/*  animation_batch_processor.h                                           */
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

#include "animation_bridge.h"
#include "skeleton_animation_pose.h"

#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"

class AnimationBatchProcessor : public RefCounted {
	GDCLASS(AnimationBatchProcessor, RefCounted);
	friend class CZAnimationBridge;
	struct Entry {
		int64_t handle = 0;
		int64_t parent = 0;
		ObjectID mixer_id;
		CZAnimationBridge::State *state = nullptr;
		LocalVector<Ref<SkeletonAnimationPose>> poses;
		AnimationMixer::AnimationCallbackModeProcess saved_mode;
		double delta = 0;
		double time = 0;
		double interval = 0;
		bool sample = true;
		bool display = true;
		bool exact = true;
		bool full_bones = false;
		bool queued = false;
		bool prepared = false;
		bool published = false;
		bool removed = false;
		bool dirty = false;
		String fallback;
	};
	struct Unit {
		uint32_t begin = 0;
		uint32_t end = 0;
	};
	LocalVector<Entry *> entries;
	HashMap<int64_t, Entry *> handles;
	LocalVector<Entry *> work;
	LocalVector<Unit> units;
	WorkerThreadPool::GroupID group = -1;
	int64_t next_handle = 1;
	int64_t frame = 0;
	int workers = 0;
	int batch_count = 0;
	int batch_size = 8;
	int fallbacks = 0;
	int failures = 0;
	int early_waits = 0;
	int recomputed = 0;
	int publish_entries = 0;
	uint64_t prepare_us = 0;
	uint64_t wait_us = 0;
	uint64_t publish_us = 0;
	uint64_t submit_total_us = 0;
	uint64_t submit_inline_us = 0;
	bool phase_diagnostics_enabled = false;
	bool pose_fastpaths_enabled = true;
	uint64_t submission_generation = 0;
	bool pending = false;
	bool publishing = false;
	bool preparing = false;
	struct Cleanup {
		LocalVector<Entry *> entries;
	};
	static LocalVector<Ref<AnimationBatchProcessor>> active;
	static Mutex cleanup_mutex;
	static LocalVector<Cleanup *> deferred_cleanup;
	static uint64_t next_submission_generation;
	static bool shutting_down;
	static void _dispose_entry(Entry *p_entry);
	int64_t _register(AnimationMixer *p_mixer, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, int64_t p_parent);
	void _execute(uint32_t p_batch, bool p_unused);
	void _prepare(Entry *p_entry);
	void _publish(Entry *p_entry);
	void _remove(Entry *p_entry);

protected:
	static void _bind_methods();

public:
	AnimationBatchProcessor();
	int64_t register_tree(AnimationTree *p_tree, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods);
	int64_t register_player(AnimationPlayer *p_player, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, int64_t p_parent = 0);
	void unregister_animation(int64_t p_handle);
	void unregister_tree(int64_t p_handle) { unregister_animation(p_handle); }
	bool queue_update(int64_t p_handle, double p_delta);
	bool queue_update_with_options(int64_t p_handle, double p_delta, bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval, bool p_force_full_bones = false);
	int64_t submit(int p_batch_size = 8, int p_max_workers = 8);
	void complete();
	void complete_batch(int64_t p_handle);
	void publish(int64_t p_handle);
	void discard(int64_t p_handle);
	void complete_and_publish();
	void finish_all() { complete_and_publish(); }
	void finish_for_dependency(AnimationMixer *p_mixer);
	static void finish_pending_frames();
	static void shutdown();
	static void drain_deferred_cleanup();
	bool is_pending() const { return pending; }
	bool was_evaluated(int64_t p_handle) const;
	Dictionary get_statistics() const;
	String get_fallback_reason(int64_t p_handle) const;
	static bool set_parameter_group(AnimationTree *p_tree, const TypedArray<StringName> &p_names, const Variant &p_value);
	static void set_skeleton_buffer(RID p_skeleton, const Vector<float> &p_buffer);
	static Dictionary get_skeleton_buffer_statistics(bool p_reset = false);
	static void set_pose_simd_enabled(bool p_enabled) { SkeletonAnimationPose::set_simd_enabled(p_enabled); }
	static bool is_pose_simd_enabled() { return SkeletonAnimationPose::is_simd_enabled(); }
	static void set_bone_lod_enabled(bool p_enabled) { SkeletonAnimationPose::set_bone_lod_enabled(p_enabled); }
	static bool is_bone_lod_enabled() { return SkeletonAnimationPose::is_bone_lod_enabled(); }
	~AnimationBatchProcessor();
};
