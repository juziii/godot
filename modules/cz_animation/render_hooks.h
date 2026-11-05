/**************************************************************************/
/*  render_hooks.h                                                        */
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

#include "core/os/os.h"

#include <atomic>

namespace CZAnimationRenderHooks {

struct NoMaterialDepthStatistics {
	const bool enabled = OS::get_singleton()->get_environment("CZ_ANIM_DISABLE_NO_MATERIAL_DEPTH_PREPASS") != "1";
	std::atomic<uint64_t> eligible_draw_calls{ 0 };
	std::atomic<uint64_t> simplified_draw_calls{ 0 };
};

inline NoMaterialDepthStatistics &get_no_material_depth_statistics() {
	static NoMaterialDepthStatistics statistics;
	return statistics;
}

// Resolve once at the render-list entrance; no per-surface environment reads.
inline bool is_no_material_depth_prepass_enabled() {
	return get_no_material_depth_statistics().enabled;
}

// One merge per nonempty render list. Both paths perform the same atomics.
inline void record_no_material_depth_draws(uint64_t p_eligible, uint64_t p_simplified) {
	if (p_eligible == 0) {
		return;
	}
	NoMaterialDepthStatistics &statistics = get_no_material_depth_statistics();
	statistics.eligible_draw_calls.fetch_add(p_eligible, std::memory_order_relaxed);
	statistics.simplified_draw_calls.fetch_add(p_simplified, std::memory_order_relaxed);
}

} // namespace CZAnimationRenderHooks
