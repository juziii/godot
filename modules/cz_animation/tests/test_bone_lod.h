/**************************************************************************/
/*  test_bone_lod.h                                                       */
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

#include "tests/test_macros.h"

#include "modules/cz_animation/bone_lod.h"

namespace TestCZBoneLod {

TEST_CASE("[CZAnimation] Bone LOD levels share parent-first continuous prefixes") {
	// Root is index 4. Descendants with longer lifetimes promote their parents.
	const LocalVector<int> parents = { 4, 0, 4, 2, -1, 1, 4, -1 };
	const LocalVector<int> thresholds = { 1, 3, 2, 2, 1, 1, 1, 3 };
	LocalVector<Transform3D> rest;
	rest.resize(parents.size());
	for (Transform3D &transform : rest) {
		transform.origin = Vector3(0, 1, 0);
	}
	CZBoneLod lod;
	REQUIRE(lod.build(parents, rest, thresholds, {}, 0));
	LocalVector<int> full_order;
	for (int i = 0; i < lod.get_active_bone_count(); i++) {
		const int bone = lod.to_original(i);
		full_order.push_back(bone);
		CHECK((parents[bone] < 0 || lod.to_compact(parents[bone]) < i));
		CHECK(lod.to_full_compact(bone) == i);
	}
	const int active_counts[] = { 8, 6, 4, 2, 2 };
	CZPoseSoA full(CZPoseSoA::SOA);
	full.resize(parents.size());
	for (int i = 0; i < full.size(); i++) {
		full.set_position(i, rest[i].origin);
	}
	for (int level = 0; level < 5; level++) {
		REQUIRE(lod.build(parents, rest, thresholds, {}, level));
		CHECK(lod.get_layout_build_count() == 1);
		CHECK(lod.get_active_bone_count() == active_counts[level]);
		for (int i = 0; i < lod.get_active_bone_count(); i++) {
			CHECK(lod.to_original(i) == full_order[i]);
			CHECK(lod.to_compact(full_order[i]) == i);
			CHECK(lod.to_full_compact(full_order[i]) == i);
		}
		for (int i = lod.get_active_bone_count(); i < int(full_order.size()); i++) {
			CHECK(lod.to_compact(full_order[i]) == -1);
			CHECK(lod.to_full_compact(full_order[i]) == i);
		}
		CZPoseSoA compact(CZPoseSoA::SOA);
		LocalVector<Transform3D> palette;
		lod.gather(full, compact);
		lod.expand_global(compact, palette);
		for (int bone = 0; bone < full.size(); bone++) {
			int depth = 1;
			for (int parent = parents[bone]; parent >= 0; parent = parents[parent]) {
				depth++;
			}
			CHECK(palette[bone].origin.is_equal_approx(Vector3(0, depth, 0)));
		}
	}
	REQUIRE(lod.build(parents, rest, thresholds, {}, 4, true));
	CHECK(lod.get_layout_build_count() == 1);
	CHECK(lod.get_active_bone_count() == full.size());
	for (int i = 0; i < full.size(); i++) {
		CHECK(lod.to_original(i) == full_order[i]);
	}
}

TEST_CASE("[CZAnimation] Bone LOD required dependencies rebuild the common prefix") {
	const LocalVector<int> parents = { -1, 0, 1, 0 };
	const LocalVector<int> thresholds = { 0, 1, 1, 2 };
	LocalVector<Transform3D> rest;
	rest.resize(parents.size());
	CZBoneLod lod;
	REQUIRE(lod.build(parents, rest, thresholds, {}, 3));
	CHECK(lod.get_active_bone_count() == 1);
	REQUIRE(lod.build(parents, rest, thresholds, { 2 }, 3));
	CHECK(lod.get_layout_build_count() == 2);
	CHECK(lod.get_active_bone_count() == 3);
	CHECK(lod.to_original(0) == 0);
	CHECK(lod.to_original(1) == 1);
	CHECK(lod.to_original(2) == 2);
	REQUIRE(lod.build(parents, rest, thresholds, { 2 }, 0));
	REQUIRE(lod.build(parents, rest, thresholds, { 2 }, 1));
	CHECK(lod.get_layout_build_count() == 2);
	CHECK(lod.get_active_bone_count() == 4);
	rest[2].origin.x = 2;
	REQUIRE(lod.build(parents, rest, thresholds, { 2 }, 2));
	CHECK(lod.get_layout_build_count() == 3);
	CHECK(lod.get_active_bone_count() == 3);
	CHECK_FALSE(lod.build(parents, rest, thresholds, { 4 }, 2));
	CHECK(lod.get_layout_build_count() == 3);
	CHECK(lod.get_active_bone_count() == 3);
	REQUIRE(lod.build(parents, rest, thresholds, {}, 3));
	CHECK(lod.get_layout_build_count() == 4);
	CHECK(lod.get_active_bone_count() == 1);
}

TEST_CASE("[CZAnimation] Bone LOD descriptors share complete inputs and isolate updates") {
	CZBoneLod::collect_unused_layouts();
	const Dictionary initial = CZBoneLod::get_cache_statistics();
	const bool shared = CZPoseTrackBinding::is_shared_track_data_enabled();
	{
		const LocalVector<int> parents = { 2, 0, -1, 2, 3 };
		const LocalVector<int> thresholds = { 1, 1, 0, 2, 2 };
		LocalVector<Transform3D> rest;
		rest.resize(parents.size());
		for (Transform3D &transform : rest) {
			transform.origin = Vector3(0, 1, 0);
		}
		CZBoneLod first, second, required, ordered, reversed, other_level, forced, other_rest, other_threshold, other_parent;
		REQUIRE(first.build(parents, rest, thresholds, {}, 2));
		REQUIRE(second.build(parents, rest, thresholds, {}, 2));
		CHECK((first.get_descriptor_identity() == second.get_descriptor_identity()) == shared);
		CHECK(first.get_active_bone_count() == 1);
		const void *second_identity = second.get_descriptor_identity();
		REQUIRE(required.build(parents, rest, thresholds, { 1 }, 2));
		CHECK(required.get_descriptor_identity() != first.get_descriptor_identity());
		CHECK(required.get_active_bone_count() == 3);
		CHECK(required.to_compact(2) < required.to_compact(0));
		CHECK(required.to_compact(0) < required.to_compact(1));
		REQUIRE(ordered.build(parents, rest, thresholds, { 1, 4 }, 2));
		REQUIRE(reversed.build(parents, rest, thresholds, { 4, 1 }, 2));
		CHECK(ordered.get_descriptor_identity() != reversed.get_descriptor_identity());
		REQUIRE(other_level.build(parents, rest, thresholds, {}, 1));
		REQUIRE(forced.build(parents, rest, thresholds, {}, 2, true));
		CHECK(other_level.get_descriptor_identity() != first.get_descriptor_identity());
		CHECK(forced.get_descriptor_identity() != first.get_descriptor_identity());
		CHECK(forced.get_active_bone_count() == int(parents.size()));
		LocalVector<Transform3D> changed_rest(rest);
		changed_rest[4].origin.x = 3;
		REQUIRE(other_rest.build(parents, changed_rest, thresholds, {}, 2));
		CHECK(other_rest.get_descriptor_identity() != first.get_descriptor_identity());
		LocalVector<int> changed_threshold(thresholds);
		changed_threshold[4] = 3;
		REQUIRE(other_threshold.build(parents, rest, changed_threshold, {}, 2));
		CHECK(other_threshold.get_descriptor_identity() != first.get_descriptor_identity());
		LocalVector<int> changed_parent(parents);
		changed_parent[4] = 0;
		REQUIRE(other_parent.build(changed_parent, rest, thresholds, {}, 2));
		CHECK(other_parent.get_descriptor_identity() != first.get_descriptor_identity());
		CZPoseSoA root(CZPoseSoA::SOA);
		root.resize(1);
		root.set_position(0, rest[2].origin);
		LocalVector<Transform3D> original_palette, changed_palette;
		second.expand_global(root, original_palette);
		other_rest.expand_global(root, changed_palette);
		CHECK(original_palette[4].origin.is_equal_approx(Vector3(0, 3, 0)));
		CHECK(changed_palette[4].origin.is_equal_approx(Vector3(3, 3, 0)));

		REQUIRE(first.build(parents, rest, thresholds, { 1 }, 2));
		CHECK((first.get_descriptor_identity() == required.get_descriptor_identity()) == shared);
		CHECK(first.get_layout_build_count() == 2);
		CHECK(second.get_descriptor_identity() == second_identity);
		CHECK(second.get_layout_build_count() == 1);
		CHECK(second.get_active_bone_count() == 1);
		REQUIRE(first.build(parents, rest, thresholds, { 1 }, 0));
		REQUIRE(first.build(parents, rest, thresholds, { 1 }, 2));
		CHECK(first.get_layout_build_count() == 2);
		CHECK(first.to_original(0) == required.to_original(0));
		CHECK(first.to_original(1) == required.to_original(1));
		CHECK(first.to_original(2) == required.to_original(2));
		if (shared) {
			CHECK(uint64_t(CZBoneLod::get_cache_statistics()["lod_layout_cache_hits"]) >= uint64_t(initial["lod_layout_cache_hits"]) + 3);
		} else {
			CHECK(uint64_t(CZBoneLod::get_cache_statistics()["lod_layout_pool_cached_entries"]) == uint64_t(initial["lod_layout_pool_cached_entries"]));
		}
	}
	if (shared) {
		CHECK(uint64_t(CZBoneLod::get_cache_statistics()["lod_layout_pool_only_entries"]) > 0);
		CHECK(uint64_t(CZBoneLod::get_cache_statistics()["lod_layout_pool_only_bytes"]) > 0);
	}
	CZBoneLod::collect_unused_layouts();
	const Dictionary collected = CZBoneLod::get_cache_statistics();
	CHECK(uint64_t(collected["lod_layout_pool_cached_entries"]) == uint64_t(initial["lod_layout_pool_cached_entries"]));
	CHECK(uint64_t(collected["lod_layout_pool_cached_bytes"]) == uint64_t(initial["lod_layout_pool_cached_bytes"]));
	CHECK(uint64_t(collected["lod_layout_pool_only_entries"]) == 0);
}

TEST_CASE("[CZAnimation] Bone LOD invalid input and pool cleanup preserve current descriptors") {
	const LocalVector<int> parents = { -1, 0, 1 };
	const LocalVector<int> thresholds = { 0, 1, 1 };
	LocalVector<Transform3D> rest;
	rest.resize(parents.size());
	rest[1].origin = Vector3(1, 0, 0);
	rest[2].origin = Vector3(0, 2, 0);
	CZBoneLod lod;
	REQUIRE(lod.build(parents, rest, thresholds, {}, 2));
	const void *identity = lod.get_descriptor_identity();
	const uint64_t bytes = lod.get_storage_bytes();
	CHECK(bytes > 0);
	CHECK_FALSE(lod.build({ 1, 0, -1 }, rest, thresholds, {}, 2));
	CHECK_FALSE(lod.build({ -1, 3, 0 }, rest, thresholds, {}, 2));
	CHECK_FALSE(lod.build({ -1, 1, 0 }, rest, thresholds, {}, 2));
	CHECK_FALSE(lod.build(parents, rest, { 0, -1, 1 }, {}, 2));
	CHECK_FALSE(lod.build(parents, {}, thresholds, {}, 2));
	CHECK_FALSE(lod.build(parents, rest, thresholds, {}, -1));
	CHECK_FALSE(lod.build(parents, rest, thresholds, { 3 }, 2));
	CHECK(lod.get_descriptor_identity() == identity);
	CHECK(lod.get_storage_bytes() == bytes);
	CHECK(lod.get_layout_build_count() == 1);
	CZBoneLod::clear_layout_cache();
	CHECK(uint64_t(CZBoneLod::get_cache_statistics()["lod_layout_pool_cached_entries"]) == 0);
	CHECK(uint64_t(CZBoneLod::get_cache_statistics()["lod_layout_pool_cached_bytes"]) == 0);
	CHECK(lod.get_descriptor_identity() == identity);
	CZPoseSoA compact(CZPoseSoA::SOA);
	compact.resize(1);
	compact.set_position(0, Vector3(0, 0, 3));
	LocalVector<Transform3D> palette;
	lod.expand_global(compact, palette);
	CHECK(palette[2].origin.is_equal_approx(Vector3(1, 2, 3)));
	REQUIRE(lod.build(parents, rest, thresholds, {}, 0));
	CHECK(lod.get_layout_build_count() == 1);
	CHECK(lod.get_active_bone_count() == 3);
	CHECK(lod.get_descriptor_identity() != identity);
}

} // namespace TestCZBoneLod
