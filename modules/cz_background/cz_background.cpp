/**************************************************************************/
/*  cz_background.cpp                                                     */
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

#include "cz_background.h"

#ifndef WINDOWS_ENABLED
#include <cstdlib>
#endif

#ifdef WINDOWS_ENABLED
#include "core/templates/vector.h"

#include "platform/windows/windows.h"

#undef FAILED // Overrides Error::FAILED.

#include <cwchar>

static bool get_desktop_name(HDESK p_desktop, Vector<WCHAR> &r_name) {
	DWORD size = 0;
	GetUserObjectInformationW(p_desktop, UOI_NAME, nullptr, 0, &size);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(WCHAR)) {
		return false;
	}
	r_name.resize((size + sizeof(WCHAR) - 1) / sizeof(WCHAR));
	if (!GetUserObjectInformationW(p_desktop, UOI_NAME, r_name.ptrw(), size, &size)) {
		return false;
	}
	return r_name[0] != L'\0';
}

bool CZBackground::is_current_thread_input_desktop() {
	HDESK thread_desktop = GetThreadDesktop(GetCurrentThreadId());
	if (thread_desktop == nullptr) {
		return false;
	}
	HDESK input_desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
	if (input_desktop == nullptr) {
		return false;
	}
	Vector<WCHAR> thread_name;
	Vector<WCHAR> input_name;
	const bool matches = get_desktop_name(thread_desktop, thread_name) && get_desktop_name(input_desktop, input_name) && wcscmp(thread_name.ptr(), input_name.ptr()) == 0;
	CloseDesktop(input_desktop);
	return matches;
}

bool CZBackground::is_current_process_foreground() {
	DWORD process_id = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &process_id);
	return process_id == GetCurrentProcessId();
}
#endif // WINDOWS_ENABLED

bool CZBackground::is_background_mode() {
	// Match the existing harness contract: presence (including an empty value)
	// selects the mode. Function-local initialization also works during startup.
#ifdef WINDOWS_ENABLED
	static const bool background_mode = []() {
		const DWORD previous_error = GetLastError();
		SetLastError(ERROR_SUCCESS);
		const bool exists = GetEnvironmentVariableW(L"GODOT_BACKGROUND_MODE", nullptr, 0) != 0 || GetLastError() == ERROR_SUCCESS;
		SetLastError(previous_error);
		return exists;
	}();
#else
	static const bool background_mode = std::getenv("GODOT_BACKGROUND_MODE") != nullptr;
#endif
	return background_mode;
}
