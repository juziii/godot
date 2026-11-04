/**************************************************************************/
/*  desktop_probe.cpp                                                     */
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

#include "desktop_probe.h"

#if defined(WINDOWS_ENABLED) && defined(THREADS_ENABLED)

#include "../cz_background.h"

#include "core/os/thread.h"
#include "core/string/ustring.h"

#include "platform/windows/windows.h"

#undef FAILED // Overrides Error::FAILED.
#undef IGNORE // WinBase.h; must not escape to other Godot headers.

namespace TestCZBackground {

namespace {

struct DesktopProbe {
	HDESK desktop = nullptr;
	PrivateDesktopResult result;

	~DesktopProbe() {
		if (desktop != nullptr) {
			CloseDesktop(desktop);
		}
	}
};

void probe_desktop(void *p_userdata) {
	DesktopProbe &probe = *static_cast<DesktopProbe *>(p_userdata);
	probe.result.attached = SetThreadDesktop(probe.desktop) != FALSE;
	if (probe.result.attached) {
		probe.result.input_desktop = CZBackground::is_current_thread_input_desktop();
	} else {
		probe.result.native_error = GetLastError();
	}
}

} // namespace

PrivateDesktopResult inspect_private_desktop() {
	const String name = "CZ-Background-Test-" + itos(GetCurrentProcessId()) + "-" + itos(GetCurrentThreadId());
	DesktopProbe probe;
	probe.desktop = CreateDesktopW((LPCWSTR)name.utf16().get_data(), nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS, nullptr);
	probe.result.created = probe.desktop != nullptr;
	if (!probe.result.created) {
		probe.result.native_error = GetLastError();
		return probe.result;
	}
	Thread worker;
	probe.result.thread_started = worker.start(probe_desktop, &probe) != Thread::UNASSIGNED_ID;
	if (probe.result.thread_started) {
		worker.wait_to_finish();
	}
	return probe.result;
}

} // namespace TestCZBackground

#endif // WINDOWS_ENABLED && THREADS_ENABLED
