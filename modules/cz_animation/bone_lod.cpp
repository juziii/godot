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

bool CZBoneLod::build(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
		const LocalVector<int> &p_first_disabled_lod, const LocalVector<int> &p_required, int p_level, bool p_force_full_bones) {
	const int count = p_parents.size();
	if (p_level < 0 || p_rest.size() != p_parents.size() || p_first_disabled_lod.size() != p_parents.size()) {
		return false;
	}
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
	for (int bone : p_required) {
		if (bone < 0 || bone >= count) {
			return false;
		}
	}
	parents = p_parents;
	order = new_order;
	retained.resize(count);
	for (int i = 0; i < count; i++) {
		retained[i] = p_force_full_bones || p_level == 0 || p_first_disabled_lod[i] == 0 || p_level < p_first_disabled_lod[i] || parents[i] < 0;
	}
	for (int bone : p_required) {
		retained[bone] = true;
	}
	// Every retained descendant requires its entire parent chain, including
	// ancestors which the profile explicitly requested to disable.
	for (int i = count - 1; i >= 0; i--) {
		const int bone = order[i];
		if (retained[bone] && parents[bone] >= 0) {
			retained[parents[bone]] = true;
		}
	}
	original_to_compact.resize(count);
	compact_to_original.clear();
	retained_ancestor.resize(count);
	ancestor_rest.resize(count);
	for (int bone : order) {
		original_to_compact[bone] = -1;
		if (retained[bone]) {
			original_to_compact[bone] = compact_to_original.size();
			compact_to_original.push_back(bone);
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

int CZBoneLod::to_compact(int p_original) const {
	ERR_FAIL_INDEX_V(p_original, int(original_to_compact.size()), -1);
	return original_to_compact[p_original];
}

int CZBoneLod::to_original(int p_compact) const {
	ERR_FAIL_INDEX_V(p_compact, int(compact_to_original.size()), -1);
	return compact_to_original[p_compact];
}

void CZBoneLod::gather(const CZPoseSoA &p_full, CZPoseSoA &r_compact) const {
	ERR_FAIL_COND(p_full.size() != int(parents.size()));
	ERR_FAIL_COND(&p_full == &r_compact);
	if (r_compact.size() != get_active_bone_count()) {
		r_compact.resize(get_active_bone_count());
	}
	for (int compact = 0; compact < get_active_bone_count(); compact++) {
		const int original = compact_to_original[compact];
		r_compact.set_position(compact, p_full.get_position(original));
		r_compact.set_rotation(compact, p_full.get_rotation(original));
		r_compact.set_scale(compact, p_full.get_scale(original));
	}
}

void CZBoneLod::expand_global(const CZPoseSoA &p_compact, LocalVector<Transform3D> &r_full_palette) const {
	ERR_FAIL_COND(p_compact.size() != get_active_bone_count());
	r_full_palette.resize(parents.size());
	for (int bone : order) {
		const int compact = original_to_compact[bone];
		if (compact >= 0) {
			const Transform3D local = p_compact.get_transform(compact);
			r_full_palette[bone] = parents[bone] < 0 ? local : r_full_palette[parents[bone]] * local;
		} else {
			r_full_palette[bone] = r_full_palette[retained_ancestor[bone]] * ancestor_rest[bone];
		}
	}
}
