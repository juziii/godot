/**************************************************************************/
/*  animation_batch_processor.cpp                                         */
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

#include "animation_batch_processor.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/profiling/profiling.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

LocalVector<Ref<AnimationBatchProcessor>> AnimationBatchProcessor::active;
Mutex AnimationBatchProcessor::cleanup_mutex;
LocalVector<AnimationBatchProcessor::Cleanup *> AnimationBatchProcessor::deferred_cleanup;
uint64_t AnimationBatchProcessor::next_submission_generation = 0;
bool AnimationBatchProcessor::shutting_down = false;
static thread_local int drain_depth = 0;
static thread_local uint64_t drain_cutoff = 0;

static bool is_bulk_skin_upload_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("CZ_ANIM_SKIN_UPLOAD_PER_BONE") != "1";
	return enabled;
}

AnimationBatchProcessor::AnimationBatchProcessor() {
	phase_diagnostics_enabled = OS::get_singleton()->get_environment("CZ_ANIM_PHASE_DIAGNOSTICS") == "1";
	pose_fastpaths_enabled = OS::get_singleton()->get_environment("CZ_ANIM_DISABLE_POSE_FASTPATHS") != "1";
}

int64_t AnimationBatchProcessor::_register(AnimationMixer *p_mixer, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, int64_t p_parent) {
	ERR_FAIL_COND_V(!Thread::is_main_thread() || publishing || preparing, 0);
	Ref<AnimationBatchProcessor> keep_alive(this);
	ERR_FAIL_NULL_V(p_mixer, 0);
	const ObjectID mixer_id = p_mixer->get_instance_id();
	complete_and_publish();
	p_mixer = ObjectDB::get_instance<AnimationMixer>(mixer_id);
	ERR_FAIL_NULL_V(p_mixer, 0);
	ERR_FAIL_COND_V(p_parent && (!handles.has(p_parent) || handles[p_parent]->parent), 0);
	for (Entry *entry : entries) {
		if (entry->mixer_id == p_mixer->get_instance_id() && !entry->removed) {
			return entry->handle;
		}
	}
	CZAnimationBridge::State *state = CZAnimationBridge::create(p_mixer, this, p_poses, p_safe_methods, p_parent ? handles[p_parent]->state : nullptr);
	ERR_FAIL_NULL_V(state, 0);
	Entry *entry = memnew(Entry);
	entry->handle = next_handle++;
	entry->parent = p_parent;
	entry->mixer_id = p_mixer->get_instance_id();
	entry->saved_mode = p_mixer->get_callback_mode_process();
	entry->state = state;
	for (int index = 0; index < p_poses.size(); index++) {
		Ref<SkeletonAnimationPose> pose = p_poses[index];
		if (pose.is_valid()) {
			entry->poses.push_back(pose);
		}
	}
	_collect_pose_statistics(entry, true);
	CZAnimationBridge::set_callback_mode(p_mixer, AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
	entries.push_back(entry);
	handles.insert(entry->handle, entry);
	return entry->handle;
}

int64_t AnimationBatchProcessor::register_tree(AnimationTree *p_tree, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods) {
	return _register(p_tree, p_poses, p_safe_methods, 0);
}

int64_t AnimationBatchProcessor::register_player(AnimationPlayer *p_player, const TypedArray<SkeletonAnimationPose> &p_poses, const PackedStringArray &p_safe_methods, int64_t p_parent) {
	return _register(p_player, p_poses, p_safe_methods, p_parent);
}

void AnimationBatchProcessor::_dispose_entry(Entry *p_entry) {
	CZAnimationBridge::discard(p_entry->state);
	AnimationMixer *mixer = CZAnimationBridge::mixer(p_entry->state);
	CZAnimationBridge::destroy(p_entry->state);
	if (mixer) {
		CZAnimationBridge::set_callback_mode(mixer, p_entry->saved_mode);
	}
	for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) {
		pose->release();
	}
	memdelete(p_entry);
}

void AnimationBatchProcessor::_remove(Entry *p_entry) {
	_collect_pose_statistics(p_entry);
	work.erase(p_entry);
	handles.erase(p_entry->handle);
	entries.erase(p_entry);
	_dispose_entry(p_entry);
}

void AnimationBatchProcessor::unregister_animation(int64_t p_handle) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<AnimationBatchProcessor> keep_alive(this);
	complete();
	for (Entry *entry : entries) {
		if (entry->handle == p_handle || entry->parent == p_handle) {
			entry->removed = true;
		}
	}
	if (!pending && !publishing && !preparing) {
		for (int index = int(entries.size()) - 1; index >= 0; index--) {
			if (entries[index]->removed) {
				_remove(entries[index]);
			}
		}
	}
}

bool AnimationBatchProcessor::queue_update(int64_t p_handle, double p_delta) {
	return queue_update_with_options(p_handle, p_delta, true, true, true, 0, 0);
}

bool AnimationBatchProcessor::queue_update_with_options(int64_t p_handle, double p_delta, bool p_sample, bool p_display, bool p_exact, double p_time, double p_interval, bool p_force_full_bones) {
	ERR_FAIL_COND_V(!Thread::is_main_thread() || pending || publishing || preparing, false);
	Entry **found = handles.getptr(p_handle);
	ERR_FAIL_NULL_V(found, false);
	Entry *entry = *found;
	ERR_FAIL_COND_V(entry->queued || entry->removed, false);
	ERR_FAIL_COND_V(!Math::is_finite(p_delta) || !Math::is_finite(p_time) || !Math::is_finite(p_interval) || p_interval < 0, false);
	entry->delta = p_delta;
	entry->sample = p_sample;
	entry->display = p_display;
	entry->exact = p_exact;
	entry->time = p_time;
	entry->interval = p_interval;
	entry->full_bones = p_force_full_bones;
	entry->queued = true;
	force_full_bones_requests_total += p_force_full_bones;
	return true;
}

void AnimationBatchProcessor::_collect_pose_statistics(Entry *p_entry, bool p_baseline) {
	PoseTotals current;
	for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) {
		current.sample_blend_us += pose->sample_blend_us_total;
		current.interpolate_us += pose->interpolate_us_total;
		current.matrix_us += pose->matrix_us_total;
		current.skin_pack_us += pose->skin_pack_us_total;
		current.copy_bytes += pose->get_pose_copy_bytes();
		current.in_place_blends += pose->in_place_blends_total;
		current.sampler_calls += pose->sampler_calls_total;
		current.sampler_blends += pose->sampler_blends_total;
		current.active_bones += pose->active_bones_total;
		current.full_bones += pose->full_bones_total;
		current.sampled_bones += pose->sampled_bones_total;
		current.displayed_bones += pose->displayed_bones_total;
	}
	if (!p_baseline) {
		pose_totals.sample_blend_us += current.sample_blend_us - p_entry->pose_snapshot.sample_blend_us;
		pose_totals.interpolate_us += current.interpolate_us - p_entry->pose_snapshot.interpolate_us;
		pose_totals.matrix_us += current.matrix_us - p_entry->pose_snapshot.matrix_us;
		pose_totals.skin_pack_us += current.skin_pack_us - p_entry->pose_snapshot.skin_pack_us;
		pose_totals.copy_bytes += current.copy_bytes - p_entry->pose_snapshot.copy_bytes;
		pose_totals.in_place_blends += current.in_place_blends - p_entry->pose_snapshot.in_place_blends;
		pose_totals.sampler_calls += current.sampler_calls - p_entry->pose_snapshot.sampler_calls;
		pose_totals.sampler_blends += current.sampler_blends - p_entry->pose_snapshot.sampler_blends;
		pose_totals.active_bones += current.active_bones - p_entry->pose_snapshot.active_bones;
		pose_totals.full_bones += current.full_bones - p_entry->pose_snapshot.full_bones;
		pose_totals.sampled_bones += current.sampled_bones - p_entry->pose_snapshot.sampled_bones;
		pose_totals.displayed_bones += current.displayed_bones - p_entry->pose_snapshot.displayed_bones;
	}
	p_entry->pose_snapshot = current;
}

void AnimationBatchProcessor::_finish_frame_statistics() {
	if (!frame_statistics_pending) {
		return;
	}
	frame_statistics_pending = false;
	completed_frames_total++;
	prepare_us_total += prepare_us;
	wait_us_total += wait_us;
	publish_us_total += publish_us;
	fallbacks_total += fallbacks;
	failures_total += failures;
	recomputed_total += recomputed;
}

void AnimationBatchProcessor::_prepare(Entry *p_entry) {
	GodotProfileZone("CZAnimation.Prepare");
	p_entry->prepared = true;
	p_entry->dirty = false;
	p_entry->fallback = String();
	for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) {
		if (!pose->prepare_frame(p_entry->sample, p_entry->display, p_entry->exact, p_entry->time, p_entry->interval, p_entry->full_bones)) {
			p_entry->prepared = false;
			p_entry->fallback = pose->get_fallback_reason();
			return;
		}
	}
	bool sample = p_entry->sample;
	bool display = p_entry->display;
	for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) {
		sample |= pose->needs_sample();
		display |= pose->needs_display();
	}
	for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) {
		if (sample && !pose->needs_sample() && !pose->prepare_frame(true, display, true, p_entry->time, p_entry->interval, p_entry->full_bones)) {
			p_entry->prepared = false;
			p_entry->fallback = pose->get_fallback_reason();
			return;
		}
	}
	p_entry->prepared = CZAnimationBridge::prepare(p_entry->state, p_entry->delta, p_entry->poses);
	if (!p_entry->prepared) {
		p_entry->fallback = CZAnimationBridge::fallback(p_entry->state);
	}
}

int64_t AnimationBatchProcessor::submit(int p_batch_size, int p_max_workers) {
	ERR_FAIL_COND_V(!Thread::is_main_thread() || pending || publishing || preparing || shutting_down || p_batch_size < 1 || p_max_workers < 1, 0);
	const uint64_t submit_start = phase_diagnostics_enabled ? OS::get_singleton()->get_ticks_usec() : 0;
	Ref<AnimationBatchProcessor> keep_alive(this);
	work.clear();
	units.clear();
	fallbacks = failures = early_waits = recomputed = publish_entries = 0;
	prepare_us = wait_us = publish_us = 0;
	submit_total_us = submit_inline_us = 0;
	const uint64_t prepare_start = OS::get_singleton()->get_ticks_usec();
	batch_size = p_batch_size;
	preparing = true;
	for (Entry *entry : entries) {
		CZAnimationBridge::reset_method_validation_statistics(entry->state);
		if (phase_diagnostics_enabled) {
			CZAnimationBridge::reset_evaluation_statistics(entry->state, true);
		}
		if (!entry->queued || entry->removed || !CZAnimationBridge::mixer(entry->state)) {
			continue;
		}
		entry->published = false;
		_prepare(entry);
		fallbacks += !entry->prepared;
	}
	preparing = false;
	prepare_us = OS::get_singleton()->get_ticks_usec() - prepare_start;
	for (Entry *entry : entries) {
		if (!entry->queued || entry->removed || !CZAnimationBridge::mixer(entry->state)) {
			continue;
		}
		if (entry->parent && handles.has(entry->parent) && handles[entry->parent]->queued) {
			continue;
		}
		Unit unit;
		unit.begin = work.size();
		work.push_back(entry);
		for (Entry *child : entries) {
			if (child->parent == entry->handle && child->queued && !child->removed) {
				work.push_back(child);
			}
		}
		unit.end = work.size();
		units.push_back(unit);
	}
	for (Entry *entry : entries) {
		entry->queued = false;
	}
	frame++;
	frame_statistics_pending = true;
	pending = !work.is_empty();
	batch_count = (units.size() + batch_size - 1) / batch_size;
	batch_task_statistics.resize(batch_count);
	for (TaskStatistics &statistics : batch_task_statistics) {
		statistics = {};
	}
	running_tasks.set(0);
	task_concurrency_peak.set(0);
	task_concurrency_peak_last = 0;
	task_statistics_pending = pending;
	workers = pending ? MIN(MIN(batch_count, p_max_workers), MAX(1, WorkerThreadPool::get_singleton()->get_thread_count())) : 0;
	if (!pending) {
		_finish_frame_statistics();
		if (phase_diagnostics_enabled) {
			submit_total_us = OS::get_singleton()->get_ticks_usec() - submit_start;
		}
		return frame;
	}
	active.push_back(Ref<AnimationBatchProcessor>(this));
	submission_generation = ++next_submission_generation;
	if (units.size() < 16 || workers <= 1) {
		workers = 1;
		const uint64_t inline_start = phase_diagnostics_enabled ? OS::get_singleton()->get_ticks_usec() : 0;
		for (int batch = 0; batch < batch_count; batch++) {
			_execute(batch, false);
		}
		if (phase_diagnostics_enabled) {
			submit_inline_us = OS::get_singleton()->get_ticks_usec() - inline_start;
		}
	} else {
		group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &AnimationBatchProcessor::_execute, false, batch_count, workers, true, "CZ animation");
	}
	if (phase_diagnostics_enabled) {
		submit_total_us = OS::get_singleton()->get_ticks_usec() - submit_start;
	}
	return frame;
}

void AnimationBatchProcessor::_execute(uint32_t p_batch, bool p_unused) {
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	task_concurrency_peak.exchange_if_greater(running_tasks.increment());
	TaskStatistics &statistics = batch_task_statistics[p_batch];
	statistics.thread_id = Thread::get_caller_id();
	statistics.main_thread = Thread::is_main_thread();
	const uint32_t unit_end = MIN(uint32_t((p_batch + 1) * batch_size), units.size());
	for (uint32_t unit = p_batch * batch_size; unit < unit_end; unit++) {
		for (uint32_t index = units[unit].begin; index < units[unit].end; index++) {
			Entry *entry = work[index];
			if (!entry->prepared) {
				continue;
			}
			if (entry->parent && handles.has(entry->parent) && !handles[entry->parent]->prepared) {
				entry->dirty = true;
				continue;
			}
			CZAnimationBridge::evaluate(entry->state);
		}
	}
	running_tasks.decrement();
	statistics.wall_us = OS::get_singleton()->get_ticks_usec() - start;
}

void AnimationBatchProcessor::complete() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	if (group != -1) {
		GodotProfileZone("CZAnimation.Wait");
		const uint64_t start = OS::get_singleton()->get_ticks_usec();
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		wait_us += OS::get_singleton()->get_ticks_usec() - start;
		group = -1;
	}
	if (task_statistics_pending) {
		// Each slot has one batch writer; read only after joining every worker.
		// Sum of execution wall intervals, including inline batches, not CPU time.
		for (const TaskStatistics &statistics : batch_task_statistics) {
			task_wall_us_total += statistics.wall_us;
			main_thread_tasks_total += statistics.main_thread;
			worker_thread_tasks_total += !statistics.main_thread;
			task_threads.insert(statistics.thread_id);
		}
		tasks_total += batch_task_statistics.size();
		task_concurrency_peak_last = task_concurrency_peak.get();
		task_concurrency_peak_total = MAX(task_concurrency_peak_total, task_concurrency_peak_last);
		parallel_submissions_total += task_concurrency_peak_last > 1;
		task_statistics_pending = false;
		for (Entry *entry : work) {
			_collect_pose_statistics(entry);
		}
	}
}

void AnimationBatchProcessor::complete_batch(int64_t p_handle) {
	if (handles.has(p_handle) && pending) {
		early_waits++;
		complete();
	}
}

void AnimationBatchProcessor::_publish(Entry *p_entry) {
	AnimationMixer *mixer = CZAnimationBridge::mixer(p_entry->state);
	if (p_entry->published || p_entry->removed || !mixer) {
		return;
	}
	if (p_entry->dirty) {
		CZAnimationBridge::discard(p_entry->state);
		for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) {
			pose->history_count = 0;
		}
		if (p_entry->parent && handles.has(p_entry->parent) && !handles[p_entry->parent]->prepared) {
			for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) {
				pose->configure_attachment(Ref<SkeletonAnimationPose>(), -1, Transform3D(), Transform3D(), false);
			}
		}
		preparing = true;
		_prepare(p_entry);
		preparing = false;
		if (p_entry->prepared) {
			CZAnimationBridge::evaluate(p_entry->state);
			recomputed++;
		}
	}
	p_entry->published = true;
	publish_entries++;
	if (p_entry->prepared) {
		if (!CZAnimationBridge::publish(p_entry->state)) {
			failures++;
		}
	} else {
		for (const Ref<SkeletonAnimationPose> &pose : p_entry->poses) {
			pose->release();
		}
		mixer->advance(p_entry->delta);
	}
	_collect_pose_statistics(p_entry);
}

void AnimationBatchProcessor::publish(int64_t p_handle) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<AnimationBatchProcessor> keep_alive(this);
	if (!pending || publishing) {
		return;
	}
	complete();
	Entry **found = handles.getptr(p_handle);
	if (found && work.has(*found)) {
		publishing = true;
		const uint64_t start = OS::get_singleton()->get_ticks_usec();
		{
			GodotProfileZone("CZAnimation.Publish");
			_publish(*found);
		}
		publish_us += OS::get_singleton()->get_ticks_usec() - start;
		publishing = false;
	}
}

void AnimationBatchProcessor::discard(int64_t p_handle) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	complete();
	Entry **found = handles.getptr(p_handle);
	if (found && pending && !(*found)->published) {
		(*found)->published = true;
		CZAnimationBridge::discard((*found)->state);
	}
}

void AnimationBatchProcessor::complete_and_publish() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Ref<AnimationBatchProcessor> keep_alive(this);
	if (!pending || publishing || preparing) {
		return;
	}
	complete();
	publishing = true;
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	{
		GodotProfileZone("CZAnimation.Publish");
		for (Entry *entry : work) {
			_publish(entry);
		}
	}
	publish_us += OS::get_singleton()->get_ticks_usec() - start;
	publishing = false;
	pending = false;
	_finish_frame_statistics();
	active.erase(Ref<AnimationBatchProcessor>(this));
	for (int index = int(entries.size()) - 1; index >= 0; index--) {
		if (entries[index]->removed || !CZAnimationBridge::mixer(entries[index]->state)) {
			_remove(entries[index]);
		}
	}
}

void AnimationBatchProcessor::finish_for_dependency(AnimationMixer *p_mixer) {
	if (!pending || preparing) {
		return;
	}
	for (Entry *entry : work) {
		if (entry->published || entry->removed || CZAnimationBridge::mixer(entry->state) != p_mixer) {
			continue;
		}
		early_waits++;
		complete();
		if (Object::cast_to<AnimationPlayer>(p_mixer)) {
			entry->dirty = true;
		} else if (!publishing) {
			publish(entry->handle);
		}
		return;
	}
}

void AnimationBatchProcessor::finish_pending_frames() {
	if (!Thread::is_main_thread()) {
		return;
	}
	if (drain_depth++ == 0) {
		drain_cutoff = next_submission_generation;
	}
	for (;;) {
		Ref<AnimationBatchProcessor> processor;
		for (int index = int(active.size()) - 1; index >= 0; index--) {
			const Ref<AnimationBatchProcessor> &candidate = active[index];
			if (!candidate->publishing && !candidate->preparing && candidate->submission_generation <= drain_cutoff) {
				processor = candidate;
				break;
			}
		}
		if (processor.is_null()) {
			break;
		}
		processor->complete_and_publish();
	}
	// A publication callback may submit another generation. Join it before a
	// resource writer returns, but defer its events to a later drain epoch. This
	// also bounds A/B callbacks that continuously submit each other's next frame.
	for (const Ref<AnimationBatchProcessor> &processor : active) {
		if (processor->publishing || processor->preparing) {
			continue;
		}
		processor->complete();
		for (Entry *entry : processor->work) {
			if (!entry->published && !entry->removed) {
				entry->dirty = true;
			}
		}
	}
	drain_deferred_cleanup();
	drain_depth--;
}

void AnimationBatchProcessor::shutdown() {
	shutting_down = true;
	finish_pending_frames();
	while (!active.is_empty()) {
		Ref<AnimationBatchProcessor> processor = active[active.size() - 1];
		processor->complete_and_publish();
	}
	drain_deferred_cleanup();
}

void AnimationBatchProcessor::drain_deferred_cleanup() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	LocalVector<Cleanup *> cleanup;
	{
		MutexLock lock(cleanup_mutex);
		SWAP(cleanup, deferred_cleanup);
	}
	for (Cleanup *payload : cleanup) {
		for (Entry *entry : payload->entries) {
			_dispose_entry(entry);
		}
		memdelete(payload);
	}
	SkeletonAnimationPose::drain_deferred_cleanup();
}

bool AnimationBatchProcessor::was_evaluated(int64_t p_handle) const {
	Entry *const *found = handles.getptr(p_handle);
	return group == -1 && found && !(*found)->removed && (*found)->prepared && (*found)->published && CZAnimationBridge::succeeded((*found)->state);
}

String AnimationBatchProcessor::get_fallback_reason(int64_t p_handle) const {
	Entry *const *found = handles.getptr(p_handle);
	return found ? (*found)->fallback : String("Invalid registration");
}

Dictionary AnimationBatchProcessor::get_statistics() const {
	Dictionary result;
	result["bulk_skin_upload_enabled"] = is_bulk_skin_upload_enabled();
	result.merge(CZPoseTrackBinding::get_track_data_statistics());
	result["sample_scratch_reuse_enabled"] = CZPoseTrackBinding::is_sample_scratch_reuse_enabled();
	result["in_place_first_blend_enabled"] = CZPoseTrackBinding::is_in_place_first_blend_enabled();
	result["zero_pose_skip_enabled"] = CZPoseTrackBinding::is_zero_pose_skip_enabled();
	result["sample_scratch_buffer_growths_total"] = CZPoseSamplingScratch::get_buffer_growths_total();
	if (Thread::is_main_thread() && group == -1 && !pending && !preparing && !publishing) {
		uint64_t binding_bytes = 0, pose_bytes = 0;
		uint64_t lod_bytes = 0;
		HashSet<const void *> registered_layouts;
		for (const Entry *entry : entries) {
			binding_bytes += CZAnimationBridge::get_binding_sample_scratch_bytes(entry->state);
			for (const Ref<SkeletonAnimationPose> &pose : entry->poses) {
				pose_bytes += pose->sampling_scratch.get_storage_bytes();
				const void *identity = pose->lod.get_descriptor_identity();
				if (!entry->removed && identity && !registered_layouts.has(identity)) {
					registered_layouts.insert(identity);
					lod_bytes += pose->lod.get_storage_bytes();
				}
			}
		}
		result["binding_sample_scratch_bytes"] = binding_bytes;
		result["pose_sample_scratch_bytes"] = pose_bytes;
		result["sample_scratch_bytes"] = binding_bytes + pose_bytes;
		result["registered_lod_layout_bytes"] = lod_bytes;
		result["registered_lod_layout_count"] = registered_layouts.size();
		result.merge(CZBoneLod::get_cache_statistics());
	}
	result["phase_diagnostics_enabled"] = phase_diagnostics_enabled;
	result["pose_fastpaths_enabled"] = pose_fastpaths_enabled;
	const bool diagnostics_ready = phase_diagnostics_enabled && Thread::is_main_thread() && frame > 0 && group == -1 && !pending && !preparing && !publishing;
	CZAnimationBridge::EvaluationStatistics evaluation_statistics;
	result["frame"] = frame;
	result["work_units"] = units.size();
	result["batches"] = batch_count;
	result["workers"] = workers;
	result["fallbacks"] = fallbacks;
	result["failed"] = failures;
	result["early_waits"] = early_waits;
	result["recomputed"] = recomputed;
	result["publish_entries"] = publish_entries;
	result["prepare_us"] = prepare_us;
	result["wait_us"] = wait_us;
	result["publish_us"] = publish_us;
	result["completed_frames_total"] = completed_frames_total;
	result["tasks_total"] = tasks_total;
	result["task_wall_us_total"] = task_wall_us_total;
	result["main_thread_tasks_total"] = main_thread_tasks_total;
	result["worker_thread_tasks_total"] = worker_thread_tasks_total;
	result["task_threads_total"] = task_threads.size();
	result["parallel_submissions_total"] = parallel_submissions_total;
	result["task_concurrency_peak_total"] = task_concurrency_peak_total;
	result["task_concurrency_peak_last"] = task_concurrency_peak_last;
	result["prepare_us_total"] = prepare_us_total;
	result["wait_us_total"] = wait_us_total;
	result["publish_us_total"] = publish_us_total;
	result["sample_blend_us_total"] = pose_totals.sample_blend_us;
	result["interpolate_us_total"] = pose_totals.interpolate_us;
	result["matrix_us_total"] = pose_totals.matrix_us;
	result["skin_pack_us_total"] = pose_totals.skin_pack_us;
	result["pose_copy_bytes_total"] = pose_totals.copy_bytes;
	result["in_place_blends_total"] = pose_totals.in_place_blends;
	result["sampler_calls_total"] = pose_totals.sampler_calls;
	result["sampler_blends_total"] = pose_totals.sampler_blends;
	result["sampler_without_blend_calls_total"] = pose_totals.sampler_calls - pose_totals.sampler_blends;
	result["fallbacks_total"] = fallbacks_total;
	result["failures_total"] = failures_total;
	result["recomputed_total"] = recomputed_total;
	result["active_bones_total"] = pose_totals.active_bones;
	result["full_bones_total"] = pose_totals.full_bones;
	result["sampled_bones_total"] = pose_totals.sampled_bones;
	result["displayed_bones_total"] = pose_totals.displayed_bones;
	result["force_full_bones_requests_total"] = force_full_bones_requests_total;
	result["pose_simd_available"] = CZPoseSoA::is_simd_available();
	result["pose_simd_enabled"] = SkeletonAnimationPose::is_simd_enabled();
	result["pose_layout_soa"] = CZPoseSoA::get_default_layout() == CZPoseSoA::SOA;
	result["pose_simd_effective"] = SkeletonAnimationPose::is_simd_enabled() && CZPoseSoA::is_simd_available() && CZPoseSoA::get_default_layout() == CZPoseSoA::SOA;
	result["bone_lod_enabled"] = SkeletonAnimationPose::is_bone_lod_enabled();
	int counts[3] = {}, fallback_counts[3] = {}, evaluated_counts[3] = {};
	int samples = 0, displays = 0, exact = 0, active_bones = 0, total_bones = 0;
	uint64_t sample_blend_us = 0, interpolate_us = 0, skin_pack_us = 0, matrix_us = 0, pose_memory_bytes = 0, copies = 0, captured_bones = 0;
	uint64_t capture_rebounds = 0, capture_input_refreshes = 0;
	uint64_t attachment_same_source_updates = 0, attachment_dependency_changes = 0, external_requirement_updates = 0;
	CZAnimationBridge::MethodValidationStatistics method_statistics;
	for (const Entry *entry : work) {
		AnimationMixer *mixer = CZAnimationBridge::mixer(entry->state);
		if (!mixer || entry->removed) {
			continue;
		}
		const int kind = entry->parent ? 2 : (Object::cast_to<AnimationPlayer>(mixer) ? 1 : 0);
		counts[kind]++;
		const CZAnimationBridge::MethodValidationStatistics validation = CZAnimationBridge::get_method_validation_statistics(entry->state);
		method_statistics.cache_hits += validation.cache_hits;
		method_statistics.scans += validation.scans;
		method_statistics.animations += validation.animations;
		method_statistics.tracks += validation.tracks;
		method_statistics.keys += validation.keys;
		if (diagnostics_ready) {
			const CZAnimationBridge::EvaluationStatistics statistics = CZAnimationBridge::get_evaluation_statistics(entry->state);
			evaluation_statistics.total_us += statistics.total_us;
			evaluation_statistics.graph_us += statistics.graph_us;
			evaluation_statistics.weights_us += statistics.weights_us;
			evaluation_statistics.pose_us += statistics.pose_us;
			evaluation_statistics.aux_blend_us += statistics.aux_blend_us;
			evaluation_statistics.calls += statistics.calls;
			evaluation_statistics.first_key_calls += statistics.first_key_calls;
			evaluation_statistics.animation_instances += statistics.animation_instances;
			evaluation_statistics.track_count_sum += statistics.track_count_sum;
			evaluation_statistics.raw_weight_elements += statistics.raw_weight_elements;
			evaluation_statistics.owned_weight_copy_elements += statistics.owned_weight_copy_elements;
			evaluation_statistics.borrowed_weight_elements += statistics.borrowed_weight_elements;
			evaluation_statistics.zero_weight_elements_inspected += statistics.zero_weight_elements_inspected;
			evaluation_statistics.borrowed_weight_calls += statistics.borrowed_weight_calls;
			evaluation_statistics.eventless_publish_calls += statistics.eventless_publish_calls;
			evaluation_statistics.eventful_instances += statistics.eventful_instances;
			evaluation_statistics.unknown_event_traits += statistics.unknown_event_traits;
			evaluation_statistics.normalized_copy_elements += statistics.normalized_copy_elements;
			evaluation_statistics.sample_calls += statistics.sample_calls;
			evaluation_statistics.sample_zero_weight_calls += statistics.sample_zero_weight_calls;
			evaluation_statistics.sample_no_written_calls += statistics.sample_no_written_calls;
			evaluation_statistics.sample_empty_binding_calls += statistics.sample_empty_binding_calls;
			evaluation_statistics.sample_certified_zero_weight_calls += statistics.sample_certified_zero_weight_calls;
			evaluation_statistics.zero_weight_mask_rebuilds += statistics.zero_weight_mask_rebuilds;
			evaluation_statistics.evaluate_instance_track_skips += statistics.evaluate_instance_track_skips;
			evaluation_statistics.publish_instance_track_skips += statistics.publish_instance_track_skips;
		}
		fallback_counts[kind] += !entry->prepared;
		evaluated_counts[kind] += was_evaluated(entry->handle);
		bool sampled = entry->sample;
		bool displayed = entry->display;
		exact += entry->exact;
		for (const Ref<SkeletonAnimationPose> &pose : entry->poses) {
			sampled |= pose->needs_sample();
			displayed |= pose->needs_display();
			active_bones += pose->get_active_bone_count();
			total_bones += pose->get_bone_count();
			if (group == -1) {
				sample_blend_us += pose->sample_blend_us;
				interpolate_us += pose->interpolate_us;
				skin_pack_us += pose->skin_pack_us;
				matrix_us += pose->matrix_us;
				pose_memory_bytes += pose->get_storage_bytes();
				copies += pose->get_skin_buffer_copies();
				captured_bones += pose->captured_bones;
				capture_rebounds += pose->capture_rebounds_total;
				capture_input_refreshes += pose->capture_input_refreshes_total;
				attachment_same_source_updates += pose->attachment_same_source_updates_total;
				attachment_dependency_changes += pose->attachment_dependency_changes_total;
				external_requirement_updates += pose->external_requirement_updates_total;
			}
		}
		if (group == -1) {
			samples += sampled;
			displays += displayed;
		}
	}
	const char *names[] = { "pawns", "animals", "weapons" };
	for (int kind = 0; kind < 3; kind++) {
		result[names[kind]] = counts[kind];
		result[String(names[kind]) + "_fallbacks"] = fallback_counts[kind];
		result[String(names[kind]) + "_evaluated"] = evaluated_counts[kind];
	}
	result["samples"] = samples;
	result["displays"] = displays;
	result["exact"] = exact;
	result["active_bones"] = active_bones;
	result["total_bones"] = total_bones;
	result["full_bones"] = total_bones;
	result["method_validation_cache_hits"] = method_statistics.cache_hits;
	result["method_validation_scans"] = method_statistics.scans;
	result["method_validation_animations"] = method_statistics.animations;
	result["method_validation_tracks"] = method_statistics.tracks;
	result["method_validation_keys"] = method_statistics.keys;
	if (group == -1) {
		// These are work totals across entries, not parallel frame wall time.
		result["sample_blend_us"] = sample_blend_us;
		result["interpolate_us"] = interpolate_us;
		result["skin_pack_us"] = skin_pack_us;
		result["matrix_us"] = matrix_us;
		result["pose_memory_bytes"] = pose_memory_bytes;
		result["skin_buffer_copies"] = copies;
		result["captured_bones"] = captured_bones;
		result["capture_rebounds_total"] = capture_rebounds;
		result["capture_input_refreshes_total"] = capture_input_refreshes;
		result["attachment_same_source_updates_total"] = attachment_same_source_updates;
		result["attachment_dependency_changes_total"] = attachment_dependency_changes;
		result["external_requirement_updates_total"] = external_requirement_updates;
	}
	if (diagnostics_ready) {
		result["submit_total_us"] = submit_total_us;
		result["submit_inline_us"] = submit_inline_us;
		result["evaluate_total_us"] = evaluation_statistics.total_us;
		result["evaluate_graph_us"] = evaluation_statistics.graph_us;
		result["evaluate_weights_us"] = evaluation_statistics.weights_us;
		result["evaluate_pose_us"] = evaluation_statistics.pose_us;
		result["evaluate_aux_blend_us"] = evaluation_statistics.aux_blend_us;
		result["evaluate_calls"] = evaluation_statistics.calls;
		result["evaluate_first_key_calls"] = evaluation_statistics.first_key_calls;
		result["evaluate_animation_instances"] = evaluation_statistics.animation_instances;
		result["evaluate_track_count_sum"] = evaluation_statistics.track_count_sum;
		result["evaluate_raw_weight_elements"] = evaluation_statistics.raw_weight_elements;
		result["evaluate_owned_weight_copy_elements"] = evaluation_statistics.owned_weight_copy_elements;
		result["evaluate_borrowed_weight_elements"] = evaluation_statistics.borrowed_weight_elements;
		result["evaluate_zero_weight_elements_inspected"] = evaluation_statistics.zero_weight_elements_inspected;
		result["evaluate_borrowed_weight_calls"] = evaluation_statistics.borrowed_weight_calls;
		result["publish_eventless_calls"] = evaluation_statistics.eventless_publish_calls;
		result["evaluate_eventful_instances"] = evaluation_statistics.eventful_instances;
		result["evaluate_unknown_event_traits"] = evaluation_statistics.unknown_event_traits;
		result["evaluate_normalized_copy_elements"] = evaluation_statistics.normalized_copy_elements;
		result["sample_pose_instance_calls"] = evaluation_statistics.sample_calls;
		result["sample_zero_bone_weight_calls"] = evaluation_statistics.sample_zero_weight_calls;
		result["sample_no_written_bone_calls"] = evaluation_statistics.sample_no_written_calls;
		result["sample_empty_binding_calls"] = evaluation_statistics.sample_empty_binding_calls;
		result["sample_certified_zero_weight_calls"] = evaluation_statistics.sample_certified_zero_weight_calls;
		result["zero_weight_mask_rebuilds"] = evaluation_statistics.zero_weight_mask_rebuilds;
		result["evaluate_instance_track_skips"] = evaluation_statistics.evaluate_instance_track_skips;
		result["publish_instance_track_skips"] = evaluation_statistics.publish_instance_track_skips;
	}
	return result;
}

bool AnimationBatchProcessor::set_parameter_group(AnimationTree *p_tree, const TypedArray<StringName> &p_names, const Variant &p_value) {
	return CZAnimationBridge::set_parameter_group(p_tree, p_names, p_value);
}

static void upload_skeleton_buffer(RID p_skeleton, const Vector<float> &p_buffer, bool p_bulk) {
	if (RSG::mesh_storage) {
		RSG::mesh_storage->skeleton_set_buffer(p_skeleton, p_buffer, p_bulk);
	}
}

void AnimationBatchProcessor::set_skeleton_buffer(RID p_skeleton, const Vector<float> &p_buffer) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	// Callable-bound Variants retain the COW buffer until the render queue consumes
	// it. RID resize, this call and free all travel through the same server queue.
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&upload_skeleton_buffer).bind(p_skeleton, p_buffer, is_bulk_skin_upload_enabled()));
}

Dictionary AnimationBatchProcessor::get_skeleton_buffer_statistics(bool p_reset) {
	return RSG::mesh_storage ? RSG::mesh_storage->skeleton_get_buffer_statistics(p_reset) : Dictionary();
}

AnimationBatchProcessor::~AnimationBatchProcessor() {
	// A pending frame owns a strong Ref until scene-thread publication finishes.
	DEV_ASSERT(!pending);
	if (Thread::is_main_thread()) {
		while (!entries.is_empty()) {
			_remove(entries[entries.size() - 1]);
		}
	} else if (!entries.is_empty()) {
		Cleanup *payload = memnew(Cleanup);
		SWAP(payload->entries, entries);
		for (Entry *entry : payload->entries) {
			CZAnimationBridge::detach_owner(entry->state);
		}
		MutexLock lock(cleanup_mutex);
		deferred_cleanup.push_back(payload);
	}
}

void AnimationBatchProcessor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("register_tree", "tree", "poses", "safe_methods"), &AnimationBatchProcessor::register_tree);
	ClassDB::bind_method(D_METHOD("register_player", "player", "poses", "safe_methods", "parent"), &AnimationBatchProcessor::register_player, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("unregister_animation", "handle"), &AnimationBatchProcessor::unregister_animation);
	ClassDB::bind_method(D_METHOD("unregister_tree", "handle"), &AnimationBatchProcessor::unregister_tree);
	ClassDB::bind_method(D_METHOD("queue_update", "handle", "delta"), &AnimationBatchProcessor::queue_update);
	ClassDB::bind_method(D_METHOD("queue_update_with_options", "handle", "delta", "sample", "display", "exact", "time", "interval", "force_full_bones"), &AnimationBatchProcessor::queue_update_with_options, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("submit", "batch_size", "max_workers"), &AnimationBatchProcessor::submit, DEFVAL(8), DEFVAL(8));
	ClassDB::bind_method(D_METHOD("complete"), &AnimationBatchProcessor::complete);
	ClassDB::bind_method(D_METHOD("complete_batch", "handle"), &AnimationBatchProcessor::complete_batch);
	ClassDB::bind_method(D_METHOD("publish", "handle"), &AnimationBatchProcessor::publish);
	ClassDB::bind_method(D_METHOD("discard", "handle"), &AnimationBatchProcessor::discard);
	ClassDB::bind_method(D_METHOD("complete_and_publish"), &AnimationBatchProcessor::complete_and_publish);
	ClassDB::bind_method(D_METHOD("finish_all"), &AnimationBatchProcessor::finish_all);
	ClassDB::bind_method(D_METHOD("is_pending"), &AnimationBatchProcessor::is_pending);
	ClassDB::bind_method(D_METHOD("was_evaluated", "handle"), &AnimationBatchProcessor::was_evaluated);
	ClassDB::bind_method(D_METHOD("get_statistics"), &AnimationBatchProcessor::get_statistics);
	ClassDB::bind_method(D_METHOD("get_fallback_reason", "handle"), &AnimationBatchProcessor::get_fallback_reason);
	ClassDB::bind_static_method("AnimationBatchProcessor", D_METHOD("set_parameter_group", "tree", "names", "value"), &AnimationBatchProcessor::set_parameter_group);
	ClassDB::bind_static_method("AnimationBatchProcessor", D_METHOD("set_skeleton_buffer", "skeleton", "buffer"), &AnimationBatchProcessor::set_skeleton_buffer);
	ClassDB::bind_static_method("AnimationBatchProcessor", D_METHOD("get_skeleton_buffer_statistics", "reset"), &AnimationBatchProcessor::get_skeleton_buffer_statistics, DEFVAL(false));
	ClassDB::bind_static_method("AnimationBatchProcessor", D_METHOD("set_pose_simd_enabled", "enabled"), &AnimationBatchProcessor::set_pose_simd_enabled);
	ClassDB::bind_static_method("AnimationBatchProcessor", D_METHOD("is_pose_simd_enabled"), &AnimationBatchProcessor::is_pose_simd_enabled);
	ClassDB::bind_static_method("AnimationBatchProcessor", D_METHOD("set_bone_lod_enabled", "enabled"), &AnimationBatchProcessor::set_bone_lod_enabled);
	ClassDB::bind_static_method("AnimationBatchProcessor", D_METHOD("is_bone_lod_enabled"), &AnimationBatchProcessor::is_bone_lod_enabled);
}
