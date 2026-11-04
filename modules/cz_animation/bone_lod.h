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
#include "core/templates/local_vector.h"

// Pure binding data. Never changes Mesh/Skin indices. A disabled subtree is
// reconstructed with rest transforms relative to its nearest retained ancestor.
class CZBoneLod {
	LocalVector<int> parents;
	LocalVector<int> order;
	LocalVector<int> original_to_compact;
	LocalVector<int> compact_to_original;
	LocalVector<int> retained_ancestor;
	LocalVector<Transform3D> ancestor_rest;
	LocalVector<uint8_t> retained;

public:
	// first_disabled_lod == 0 means always retained. Required bones are supplied
	// by binding (root motion, modifiers, sockets, queries and cross-pose copies).
	// Build validates the complete hierarchy before changing this instance.
	bool build(const LocalVector<int> &p_parents, const LocalVector<Transform3D> &p_rest,
			const LocalVector<int> &p_first_disabled_lod, const LocalVector<int> &p_required,
			int p_level, bool p_force_full_bones = false);
	int get_bone_count() const { return parents.size(); }
	int get_active_bone_count() const { return compact_to_original.size(); }
	int to_compact(int p_original) const;
	int to_original(int p_compact) const;
	void gather(const CZPoseSoA &p_full, CZPoseSoA &r_compact) const;
	void expand_global(const CZPoseSoA &p_compact, LocalVector<Transform3D> &r_full_palette) const;
};
