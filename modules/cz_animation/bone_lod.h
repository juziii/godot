/**************************************************************************/
/*  bone_lod.h                                                            */
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

#include "pose_soa.h"

#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

// Pure binding data. Never changes Mesh/Skin indices. A disabled subtree is
// reconstructed with rest transforms relative to its nearest retained ancestor.
class CZBoneLod {
	struct LayoutData : RefCounted {
		LocalVector<int> parents;
		LocalVector<int> order;
		LocalVector<int> original_to_compact;
		LocalVector<int> retained_ancestor;
		LocalVector<Transform3D> ancestor_rest;
		LocalVector<uint8_t> retained;
		LocalVector<Transform3D> source_rest;
		LocalVector<int> source_thresholds;
		LocalVector<int> source_required;
		LocalVector<int> effective_thresholds;
		int active_bone_count = 0;
		int level = 0;
		bool force_full_bones = false;

		bool matches_inputs(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
				const LocalVector<int> &p_thresholds, const LocalVector<int> &p_required) const;
		bool build(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
				const LocalVector<int> &p_thresholds, const LocalVector<int> &p_required,
				int p_level, bool p_force_full_bones, const LayoutData *p_previous);
		uint64_t get_storage_bytes() const;
	};
	Ref<LayoutData> data;
	uint64_t layout_build_count = 0;
	static HashMap<uint32_t, LocalVector<Ref<LayoutData>>> shared_layouts;
	static uint64_t descriptor_builds;
	static uint64_t cache_hits;
	static uint64_t cache_misses;
	static uint32_t hash_inputs(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
			const LocalVector<int> &p_thresholds, const LocalVector<int> &p_required, int p_level, bool p_force_full_bones);

public:
	// first_disabled_lod == 0 means always retained. Required bones are supplied
	// by binding (root motion, modifiers, sockets, queries and cross-pose copies).
	// Levels use prefixes of one parent-first order. Build reuses that order
	// across level/force-full changes; changed binding inputs rebuild it only
	// after validating the complete hierarchy.
	bool build(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
			const LocalVector<int> &p_first_disabled_lod, const LocalVector<int> &p_required,
			int p_level, bool p_force_full_bones = false);
	int get_bone_count() const { return data.is_valid() ? data->parents.size() : 0; }
	int get_active_bone_count() const { return data.is_valid() ? data->active_bone_count : 0; }
	uint64_t get_layout_build_count() const { return layout_build_count; }
	uint64_t get_storage_bytes() const;
	const void *get_descriptor_identity() const { return data.ptr(); }
	static Dictionary get_cache_statistics();
	static void collect_unused_layouts();
	static void clear_layout_cache();
	int to_compact(int p_original) const;
	int to_full_compact(int p_original) const;
	int to_original(int p_compact) const;
	void gather(const CZPoseSoA &p_full, CZPoseSoA &r_compact) const;
	void expand_global(const CZPoseSoA &p_compact, LocalVector<Transform3D> &r_full_palette) const;
};
