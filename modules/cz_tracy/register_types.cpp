/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "core/profiling/profiling.h"

#ifdef GODOT_USE_TRACY
#include <tracy/TracyC.h>

#include <cstring>

#ifdef WINDOWS_ENABLED
#define CZ_TRACY_EXPORT extern "C" __declspec(dllexport)
#else
#define CZ_TRACY_EXPORT extern "C" __attribute__((visibility("default")))
#endif

CZ_TRACY_EXPORT int32_t godot_csharp_tracy_bridge_version() {
	return 1;
}

CZ_TRACY_EXPORT const void *godot_csharp_tracy_source_location(const char *p_name, int32_t p_name_length) {
	if (p_name == nullptr || p_name_length <= 0) {
		return nullptr;
	}
	const StringName name(String::utf8(p_name, p_name_length));
	static const StringName file("C#");
	static const StringName function("ColonyZero");
	const uintptr_t identity = name.hash() == 0 ? 1 : name.hash();
	return tracy::intern_source_location(reinterpret_cast<const void *>(identity), file, function, name, 0, true);
}

CZ_TRACY_EXPORT uint64_t godot_csharp_tracy_zone_begin(const void *p_source_location) {
	if (p_source_location == nullptr) {
		return 0;
	}
	const auto *source_location = reinterpret_cast<const ___tracy_source_location_data *>(p_source_location);
	const TracyCZoneCtx context = ___tracy_emit_zone_begin(source_location, 1);
	static_assert(sizeof(context) <= sizeof(uint64_t));
	uint64_t token = 0;
	std::memcpy(&token, &context, sizeof(context));
	return token;
}

CZ_TRACY_EXPORT void godot_csharp_tracy_zone_end(uint64_t p_token) {
	if (p_token == 0) {
		return;
	}
	TracyCZoneCtx context = {};
	static_assert(sizeof(context) <= sizeof(uint64_t));
	std::memcpy(&context, &p_token, sizeof(context));
	___tracy_emit_zone_end(context);
}

#undef CZ_TRACY_EXPORT
#endif // GODOT_USE_TRACY

void initialize_cz_tracy_module(ModuleInitializationLevel p_level) {
}

void uninitialize_cz_tracy_module(ModuleInitializationLevel p_level) {
}
