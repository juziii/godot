/**************************************************************************/
/*  bone_lod.cpp                                                          */
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

#include "bone_lod.h"

#include "core/os/thread.h"
#include "core/templates/hashfuncs.h"

HashMap<uint32_t, LocalVector<Ref<CZBoneLod::LayoutData>>> CZBoneLod::shared_layouts;
uint64_t CZBoneLod::descriptor_builds = 0;
uint64_t CZBoneLod::cache_hits = 0;
uint64_t CZBoneLod::cache_misses = 0;

template <typename T>
static bool same_lod_input(const LocalVector<T> &p_a, const LocalVector<T> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (uint32_t i = 0; i < p_a.size(); i++) {
		if (p_a[i] != p_b[i]) {
			return false;
		}
	}
	return true;
}

struct CZLodBoneOrder {
	int bone;
	int threshold;
	int topology_index;

	bool operator<(const CZLodBoneOrder &p_other) const {
		if (threshold != p_other.threshold) {
			return threshold == 0 || (p_other.threshold != 0 && threshold > p_other.threshold);
		}
		return topology_index < p_other.topology_index;
	}
};

bool CZBoneLod::LayoutData::matches_inputs(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
		const LocalVector<int> &p_thresholds, const LocalVector<int> &p_required) const {
	return same_lod_input(parents, p_parents) && same_lod_input(source_rest, p_rest) &&
			same_lod_input(source_thresholds, p_thresholds) && same_lod_input(source_required, p_required);
}

bool CZBoneLod::LayoutData::build(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
		const LocalVector<int> &p_first_disabled_lod, const LocalVector<int> &p_required, int p_level, bool p_force_full_bones,
		const LayoutData *p_previous) {
	const int count = p_parents.size();
	if (p_previous) {
		parents = p_previous->parents;
		order = p_previous->order;
		source_rest = p_previous->source_rest;
		source_thresholds = p_previous->source_thresholds;
		source_required = p_previous->source_required;
		effective_thresholds = p_previous->effective_thresholds;
	} else {
		LocalVector<int> new_order;
		LocalVector<uint8_t> state;
		LocalVector<int> path;
		state.resize(count);
		for (int i = 0; i < count; i++) {
			state[i] = 0;
			if (p_parents[i] < -1 || p_parents[i] >= count || p_parents[i] == i || p_first_disabled_lod[i] < 0) {
				return false;
			}
		}
		// Parent-first order without requiring original indices to be topological.
		for (int i = 0; i < count; i++) {
			int bone = i;
			path.clear();
			while (bone >= 0 && state[bone] != 2) {
				if (state[bone] == 1) {
					return false;
				}
				state[bone] = 1;
				path.push_back(bone);
				bone = p_parents[bone];
			}
			while (!path.is_empty()) {
				bone = path[path.size() - 1];
				path.resize(path.size() - 1);
				state[bone] = 2;
				new_order.push_back(bone);
			}
		}
		LocalVector<int> thresholds(p_first_disabled_lod);
		for (int i = 0; i < count; i++) {
			if (p_parents[i] < 0) {
				thresholds[i] = 0;
			}
		}
		for (int bone : p_required) {
			thresholds[bone] = 0;
		}
		// A parent's lifetime includes every retained descendant. Sorting by this
		// lifetime makes every LOD a prefix; topological ties keep parents first.
		for (int i = count - 1; i >= 0; i--) {
			const int bone = new_order[i];
			const int parent = p_parents[bone];
			if (parent >= 0 && thresholds[parent] != 0) {
				thresholds[parent] = thresholds[bone] == 0 ? 0 : MAX(thresholds[parent], thresholds[bone]);
			}
		}
		LocalVector<CZLodBoneOrder> sorted;
		for (int i = 0; i < count; i++) {
			sorted.push_back({ new_order[i], thresholds[new_order[i]], i });
		}
		sorted.sort();
		for (int i = 0; i < count; i++) {
			new_order[i] = sorted[i].bone;
		}
		parents = p_parents;
		order = new_order;
		source_rest = p_rest;
		source_thresholds = p_first_disabled_lod;
		source_required = p_required;
		effective_thresholds = thresholds;
	}
	level = p_level;
	force_full_bones = p_force_full_bones;
	retained.resize(count);
	active_bone_count = 0;
	for (int i = 0; i < count; i++) {
		retained[i] = p_force_full_bones || p_level == 0 || effective_thresholds[i] == 0 || p_level < effective_thresholds[i];
	}
	original_to_compact.resize(count);
	retained_ancestor.resize(count);
	ancestor_rest.resize(count);
	int full_compact = 0;
	for (int bone : order) {
		original_to_compact[bone] = full_compact++;
		if (retained[bone]) {
			active_bone_count++;
			retained_ancestor[bone] = bone;
			ancestor_rest[bone] = Transform3D();
		} else {
			const int parent = parents[bone];
			retained_ancestor[bone] = retained_ancestor[parent];
			ancestor_rest[bone] = ancestor_rest[parent] * p_rest[bone];
		}
	}
	return true;
}

uint32_t CZBoneLod::hash_inputs(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
		const LocalVector<int> &p_thresholds, const LocalVector<int> &p_required, int p_level, bool p_force_full_bones) {
	uint32_t hash = hash_murmur3_one_32(p_level);
	hash = hash_murmur3_one_32(p_force_full_bones, hash);
	for (const LocalVector<int> *input : { &p_parents, &p_thresholds, &p_required }) {
		hash = hash_murmur3_one_32(input->size(), hash);
		for (int value : *input) {
			hash = hash_murmur3_one_32(value, hash);
		}
	}
	hash = hash_murmur3_one_32(p_rest.size(), hash);
	for (const Transform3D &rest : p_rest) {
		for (int row = 0; row < 3; row++) {
			for (int column = 0; column < 3; column++) {
				hash = hash_murmur3_one_real(rest.basis[row][column], hash);
			}
			hash = hash_murmur3_one_real(rest.origin[row], hash);
		}
	}
	return hash_fmix32(hash);
}

bool CZBoneLod::build(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
		const LocalVector<int> &p_first_disabled_lod, const LocalVector<int> &p_required, int p_level, bool p_force_full_bones) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	const int count = p_parents.size();
	if (p_level < 0 || p_rest.size() != p_parents.size() || p_first_disabled_lod.size() != p_parents.size()) {
		return false;
	}
	for (int bone : p_required) {
		if (bone < 0 || bone >= count) {
			return false;
		}
	}
	const bool same_layout = data.is_valid() && data->matches_inputs(p_parents, p_rest, p_first_disabled_lod, p_required);
	if (same_layout && data->level == p_level && data->force_full_bones == p_force_full_bones) {
		return true;
	}
	const bool shared = CZPoseTrackBinding::is_shared_track_data_enabled();
	const uint32_t hash = shared ? hash_inputs(p_parents, p_rest, p_first_disabled_lod, p_required, p_level, p_force_full_bones) : 0;
	if (shared) {
		const LocalVector<Ref<LayoutData>> *candidates = shared_layouts.getptr(hash);
		if (candidates) {
			for (const Ref<LayoutData> &candidate : *candidates) {
				if (candidate->level == p_level && candidate->force_full_bones == p_force_full_bones &&
						candidate->matches_inputs(p_parents, p_rest, p_first_disabled_lod, p_required)) {
					data = candidate;
					layout_build_count += !same_layout;
					cache_hits++;
					collect_unused_layouts();
					return true;
				}
			}
		}
	}
	Ref<LayoutData> next;
	next.instantiate();
	descriptor_builds++;
	if (!next->build(p_parents, p_rest, p_first_disabled_lod, p_required, p_level, p_force_full_bones, same_layout ? data.ptr() : nullptr)) {
		return false;
	}
	data = next;
	layout_build_count += !same_layout;
	if (shared) {
		cache_misses++;
		shared_layouts[hash].push_back(data);
		collect_unused_layouts();
	}
	return true;
}

int CZBoneLod::to_compact(int p_original) const {
	const int compact = to_full_compact(p_original);
	return compact < get_active_bone_count() ? compact : -1;
}

int CZBoneLod::to_full_compact(int p_original) const {
	ERR_FAIL_INDEX_V(p_original, get_bone_count(), -1);
	return data->original_to_compact[p_original];
}

int CZBoneLod::to_original(int p_compact) const {
	ERR_FAIL_INDEX_V(p_compact, get_active_bone_count(), -1);
	return data->order[p_compact];
}

uint64_t CZBoneLod::LayoutData::get_storage_bytes() const {
	return sizeof(LayoutData) + uint64_t(parents.get_capacity() + order.get_capacity() + original_to_compact.get_capacity() + retained_ancestor.get_capacity() + source_thresholds.get_capacity() + source_required.get_capacity() + effective_thresholds.get_capacity()) * sizeof(int) +
			uint64_t(ancestor_rest.get_capacity() + source_rest.get_capacity()) * sizeof(Transform3D) + retained.get_capacity() * sizeof(uint8_t);
}

uint64_t CZBoneLod::get_storage_bytes() const {
	return data.is_valid() ? data->get_storage_bytes() : 0;
}

Dictionary CZBoneLod::get_cache_statistics() {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), Dictionary());
	uint64_t cached_bytes = 0, cached_entries = 0, pool_only_bytes = 0, pool_only_entries = 0;
	for (const KeyValue<uint32_t, LocalVector<Ref<LayoutData>>> &entry : shared_layouts) {
		for (const Ref<LayoutData> &layout : entry.value) {
			const uint64_t bytes = layout->get_storage_bytes();
			cached_bytes += bytes;
			cached_entries++;
			if (layout->get_reference_count() == 1) {
				pool_only_bytes += bytes;
				pool_only_entries++;
			}
		}
	}
	Dictionary result;
	result["lod_layout_pool_cached_bytes"] = cached_bytes;
	result["lod_layout_pool_cached_entries"] = cached_entries;
	result["lod_layout_pool_only_bytes"] = pool_only_bytes;
	result["lod_layout_pool_only_entries"] = pool_only_entries;
	result["lod_layout_descriptor_builds"] = descriptor_builds;
	result["lod_layout_cache_hits"] = cache_hits;
	result["lod_layout_cache_misses"] = cache_misses;
	return result;
}

void CZBoneLod::collect_unused_layouts() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	LocalVector<uint32_t> empty;
	for (KeyValue<uint32_t, LocalVector<Ref<LayoutData>>> &entry : shared_layouts) {
		for (int i = int(entry.value.size()) - 1; i >= 0; i--) {
			if (entry.value[i]->get_reference_count() == 1) {
				entry.value.remove_at(i);
			}
		}
		if (entry.value.is_empty()) {
			empty.push_back(entry.key);
		}
	}
	for (uint32_t key : empty) {
		shared_layouts.erase(key);
	}
}

void CZBoneLod::clear_layout_cache() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	shared_layouts.clear();
}

void CZBoneLod::gather(const CZPoseSoA &p_full, CZPoseSoA &r_compact) const {
	ERR_FAIL_COND(p_full.size() != get_bone_count());
	ERR_FAIL_COND(&p_full == &r_compact);
	if (r_compact.size() != get_active_bone_count()) {
		r_compact.resize(get_active_bone_count());
	}
	for (int compact = 0; compact < get_active_bone_count(); compact++) {
		const int original = data->order[compact];
		r_compact.set_position(compact, p_full.get_position(original));
		r_compact.set_rotation(compact, p_full.get_rotation(original));
		r_compact.set_scale(compact, p_full.get_scale(original));
	}
}

void CZBoneLod::expand_global(const CZPoseSoA &p_compact, LocalVector<Transform3D> &r_full_palette) const {
	ERR_FAIL_COND(p_compact.size() != get_active_bone_count());
	r_full_palette.resize(get_bone_count());
	if (data.is_null()) {
		return;
	}
	for (int bone : data->order) {
		const int compact = data->original_to_compact[bone];
		if (compact < data->active_bone_count) {
			const Transform3D local = p_compact.get_transform(compact);
			r_full_palette[bone] = data->parents[bone] < 0 ? local : r_full_palette[data->parents[bone]] * local;
		} else {
			r_full_palette[bone] = r_full_palette[data->retained_ancestor[bone]] * data->ancestor_rest[bone];
		}
	}
}
