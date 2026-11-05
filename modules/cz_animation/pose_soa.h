/**************************************************************************/
/*  pose_soa.h                                                            */
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

#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "scene/resources/animation.h"

// Every hot pose (rest, sampled, blended and interpolation history) uses the
// process layout. SoA padding is initialized for four-wide kernels.
class CZPoseSoA {
public:
	enum Layout {
		SOA,
		AOS,
	};

	enum Component {
		POSITION_X,
		POSITION_Y,
		POSITION_Z,
		ROTATION_X,
		ROTATION_Y,
		ROTATION_Z,
		ROTATION_W,
		SCALE_X,
		SCALE_Y,
		SCALE_Z,
		COMPONENT_COUNT,
	};

private:
	struct BonePose {
		Vector3 position;
		Quaternion rotation;
		Vector3 scale = Vector3(1, 1, 1);
	};
	Layout layout;
	int bone_count = 0;
	uint64_t copy_bytes = 0;
	LocalVector<real_t> components[COMPONENT_COUNT];
	LocalVector<BonePose> bone_poses;

public:
	explicit CZPoseSoA(Layout p_layout = get_default_layout()) :
			layout(p_layout) {}
	static Layout get_default_layout();
	Layout get_layout() const { return layout; }
	uint64_t get_copy_bytes() const { return copy_bytes; }
	void resize(int p_bone_count);
	int size() const { return bone_count; }
	int padded_size() const { return (bone_count + 3) & ~3; }
	real_t *data(Component p_component) {
		ERR_FAIL_COND_V(layout != SOA, nullptr);
		return components[p_component].ptr();
	}
	const real_t *data(Component p_component) const {
		ERR_FAIL_COND_V(layout != SOA, nullptr);
		return components[p_component].ptr();
	}
	void copy_from(const CZPoseSoA &p_source);
	void set_position(int p_bone, const Vector3 &p_value);
	void set_rotation(int p_bone, const Quaternion &p_value);
	void set_scale(int p_bone, const Vector3 &p_value);
	Vector3 get_position(int p_bone) const;
	Quaternion get_rotation(int p_bone) const;
	Vector3 get_scale(int p_bone) const;
	Transform3D get_transform(int p_bone) const;

	// Weight arrays include initialized padding. Process one graph instance at a
	// time so quaternion accumulation retains the official graph's blend order.
	void blend(const CZPoseSoA &p_sample, const CZPoseSoA &p_rest,
			const real_t *p_position_weights, const real_t *p_rotation_weights,
			const real_t *p_scale_weights, bool p_simd, bool p_rest_base = false);
	void interpolate(const CZPoseSoA &p_from, const CZPoseSoA &p_to, real_t p_weight, bool p_simd);
	static bool is_simd_available();
	uint64_t get_storage_bytes() const;
};

struct CZPoseSamplingScratch {
	LocalVector<real_t> position_weights;
	LocalVector<real_t> rotation_weights;
	LocalVector<real_t> scale_weights;

private:
	static SafeNumeric<uint64_t> buffer_growths_total;

public:
	void resize(int p_bone_count);
	uint64_t get_storage_bytes() const;
	static uint64_t get_buffer_growths_total() { return buffer_growths_total.get(); }
};

// Bind once per Animation/binding version. Indices refer directly to compact
// pose indices; unsupported/root-motion tracks remain on the official path.
class CZPoseTrackBinding {
public:
	struct SamplingStatistics {
		uint64_t calls = 0;
		uint64_t zero_weight_calls = 0;
		uint64_t no_written_calls = 0;
		uint64_t empty_binding_calls = 0;
		uint64_t certified_zero_weight_calls = 0;
	};
	struct Track {
		int animation_track = -1;
		int compact_bone = -1;
		int blend_index = -1;
	};

private:
	class TrackData : public RefCounted {
	public:
		int compact_bone_count = 0; // Complete binding order; sampling accepts its active prefix.
		LocalVector<Track> position_tracks;
		LocalVector<Track> rotation_tracks;
		LocalVector<Track> scale_tracks;
		int maximum_blend_index = -1;
		bool blend_indices_valid = true;
		TrackData();
		~TrackData() override;
		uint32_t hash() const;
		bool matches(const TrackData &p_other) const;
		uint64_t get_storage_bytes() const;
	};
	static HashSet<TrackData *> live_track_data;
	static HashMap<uint32_t, LocalVector<Ref<TrackData>>> shared_track_data;
	static uint64_t descriptor_builds;
	static uint64_t descriptor_cache_hits;
	static uint64_t descriptor_cache_misses;
	Ref<Animation> animation;
	Ref<TrackData> track_data;
	CZPoseSamplingScratch sampling_scratch;

	void _make_track_data_unique();
	static real_t _weight(const Track &p_track, real_t p_weight, const LocalVector<real_t> *p_track_weights);

public:
	static bool is_shared_track_data_enabled();
	static bool is_sample_scratch_reuse_enabled();
	static bool is_in_place_first_blend_enabled();
	static bool is_zero_pose_skip_enabled();
	static Dictionary get_track_data_statistics();
	static void collect_unused_track_data();
	static void clear_track_data_cache();
	void reset(const Ref<Animation> &p_animation, int p_compact_bone_count);
	void add_track(Animation::TrackType p_type, int p_animation_track, int p_compact_bone, int p_blend_index);
	void finalize();
	bool shares_track_data_with(const CZPoseTrackBinding &p_other) const { return track_data.is_valid() && track_data == p_other.track_data; }
	uint64_t get_sample_scratch_storage_bytes() const { return sampling_scratch.get_storage_bytes(); }
	bool sample_and_blend(double p_time, real_t p_weight, const LocalVector<real_t> *p_track_weights,
			real_t p_motion_scale, const CZPoseSoA &p_rest, CZPoseSoA &r_sample, CZPoseSoA &r_blended, bool p_simd,
			bool p_fastpaths = true, SamplingStatistics *r_statistics = nullptr, bool p_zero_weights_certified = false,
			CZPoseSamplingScratch *p_sampling_scratch = nullptr, bool p_first_contribution = false);
};
