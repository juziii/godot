/**************************************************************************/
/*  test_resource_cache.cpp                                               */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_resource_cache)

#ifdef THREADS_ENABLED

#include "core/io/resource.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_MONO_ENABLED
#include "modules/mono/csharp_script.h"
#include "modules/mono/mono_gd/gd_mono_cache.h"
#endif

namespace TestResourceCache {

struct CacheRaceState {
	Semaphore prepare_entered;
	Semaphore prepare_release;
	Semaphore cleanup_entered;
	Semaphore destructor_entered;
	Semaphore destructor_release;
	SafeFlag block_prepare;
	SafeFlag prepare_finished;
	SafeFlag destructing;
	SafeFlag prepare_during_destruction;
	SafeNumeric<uint32_t> prepare_calls;
	String path;
	Ref<Resource> owner;

	~CacheRaceState() {
		prepare_release.post();
		destructor_release.post();
	}
};

class GatedScriptInstance : public PlaceHolderScriptInstance {
	CacheRaceState &state;

public:
	bool refcount_prepare_cached_reference() override {
		state.prepare_calls.increment();
		if (state.destructing.is_set()) {
			state.prepare_during_destruction.set();
		}
		state.prepare_entered.post();
		if (state.block_prepare.is_set()) {
			state.prepare_release.wait();
		}
		state.prepare_finished.set();
		return false;
	}

	void notification(int p_notification, bool p_reversed = false) override {
		if (p_notification == Object::NOTIFICATION_PREDELETE_CLEANUP) {
			state.cleanup_entered.post();
		}
	}

	GatedScriptInstance(Object *p_owner, CacheRaceState &p_state) :
			PlaceHolderScriptInstance(nullptr, Ref<Script>(), p_owner),
			state(p_state) {}

	~GatedScriptInstance() override {
		state.destructing.set();
		state.destructor_entered.post();
		state.destructor_release.wait();
	}
};

static void destroy_resource(void *p_userdata) {
	static_cast<CacheRaceState *>(p_userdata)->owner.unref();
}

static void query_resource(void *p_userdata) {
	CacheRaceState &state = *static_cast<CacheRaceState *>(p_userdata);
	ResourceCache::get_ref(state.path);
}

static void setup_resource(CacheRaceState &r_state, const String &p_path) {
	r_state.path = p_path;
	r_state.owner.instantiate();
	r_state.owner->set_script_instance(memnew(GatedScriptInstance(r_state.owner.ptr(), r_state)));
	r_state.owner->set_path(p_path);
}

TEST_CASE("[ResourceCache] Predelete removes a cached resource before script destruction") {
	bool enumerate = false;
	SUBCASE("get_ref") {
		enumerate = false;
	}
	SUBCASE("get_cached_resources") {
		enumerate = true;
	}
	CacheRaceState state;
	setup_resource(state, "res://__resource_cache_destructor_race__.tres");
	Thread destroy_thread;
	REQUIRE(destroy_thread.start(destroy_resource, &state) != Thread::UNASSIGNED_ID);
	state.destructor_entered.wait();
	// The script destructor has entered, but its storage remains behind a gate.
	if (enumerate) {
		List<Ref<Resource>> resources;
		ResourceCache::get_cached_resources(&resources);
	} else {
		CHECK(ResourceCache::get_ref(state.path).is_null());
	}
	CHECK_EQ(state.prepare_calls.get(), 0);
	CHECK_FALSE(state.prepare_during_destruction.is_set());
	Ref<Resource> replacement;
	replacement.instantiate();
	replacement->set_path(state.path);
	state.destructor_release.post();
	destroy_thread.wait_to_finish();
	CHECK(ResourceCache::get_ref(state.path) == replacement);
}

TEST_CASE("[ResourceCache] Cache preparation excludes script destruction") {
	CacheRaceState state;
	state.block_prepare.set();
	setup_resource(state, "res://__resource_cache_prepare_race__.tres");
	Thread query_thread;
	Thread destroy_thread;
	REQUIRE(query_thread.start(query_resource, &state) != Thread::UNASSIGNED_ID);
	state.prepare_entered.wait();
	if (destroy_thread.start(destroy_resource, &state) == Thread::UNASSIGNED_ID) {
		state.prepare_release.post();
		query_thread.wait_to_finish();
		FAIL_CHECK("Could not start the resource destruction thread.");
		return;
	}
	state.cleanup_entered.wait();
	// Cleanup has started. Its cache lock must prevent it reaching the script
	// until preparation finishes; use a bounded observation window.
	bool destructor_started_early = false;
	for (int i = 0; i < 100; i++) {
		if (state.destructor_entered.try_wait()) {
			destructor_started_early = true;
			break;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK_FALSE(destructor_started_early);
	state.prepare_release.post();
	query_thread.wait_to_finish();
	if (!destructor_started_early) {
		state.destructor_entered.wait();
	}
	CHECK(state.prepare_finished.is_set());
	state.destructor_release.post();
	destroy_thread.wait_to_finish();
	CHECK_FALSE(state.prepare_during_destruction.is_set());
	CHECK(ResourceCache::get_ref(state.path).is_null());
}

TEST_CASE("[ResourceCache] Uncached destruction does not wait for cache preparation") {
	CacheRaceState cached;
	cached.block_prepare.set();
	setup_resource(cached, "res://__resource_cache_uncached_retirement__.tres");
	CacheRaceState uncached;
	setup_resource(uncached, String());
	Thread query_thread;
	Thread destroy_thread;
	REQUIRE(query_thread.start(query_resource, &cached) != Thread::UNASSIGNED_ID);
	cached.prepare_entered.wait();
	if (destroy_thread.start(destroy_resource, &uncached) == Thread::UNASSIGNED_ID) {
		cached.prepare_release.post();
		query_thread.wait_to_finish();
		FAIL_CHECK("Could not start the uncached resource destruction thread.");
		return;
	}
	uncached.cleanup_entered.wait();
	bool destructor_started_without_cache = false;
	for (int i = 0; i < 100; i++) {
		if (uncached.destructor_entered.try_wait()) {
			destructor_started_without_cache = true;
			break;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(destructor_started_without_cache);
	// Release the cache even on failure so both workers can always be joined.
	cached.prepare_release.post();
	query_thread.wait_to_finish();
	if (!destructor_started_without_cache) {
		uncached.destructor_entered.wait();
	}
	uncached.destructor_release.post();
	destroy_thread.wait_to_finish();
	CHECK(cached.prepare_finished.is_set());
	CHECK_EQ(uncached.prepare_calls.get(), 0);
}

#ifdef MODULE_MONO_ENABLED

// The native tests do not initialize a CLR. Use synthetic handles only at the
// managed callback boundary while exercising real CSharpInstance transitions.
class CSharpCacheTestAccess {
public:
	static void initialize(CSharpInstance *p_instance, Object *p_owner) {
		p_instance->owner = p_owner;
		p_instance->base_ref_counted = true;
		p_instance->gchandle = MonoGCHandleData({ reinterpret_cast<void *>(uintptr_t(1)) }, gdmono::GCHandleType::STRONG_HANDLE);
	}

	static void clear_handle(CSharpInstance *p_instance) {
		p_instance->gchandle.handle = { nullptr };
	}

	static bool has_strong_handle(CSharpInstance *p_instance) {
		return !p_instance->gchandle.is_released() && !p_instance->gchandle.is_weak();
	}

	static bool try_handle_lock() {
		Mutex &mutex = CSharpLanguage::get_singleton()->script_gchandle_release_mutex;
		if (!mutex.try_lock()) {
			return false;
		}
		mutex.unlock();
		return true;
	}
};

struct CSharpReferenceRaceState {
	Semaphore weak_swap_entered;
	Semaphore weak_swap_release;
	Semaphore query_started;
	SafeFlag block_weak_swap;
	SafeFlag query_finished;
	SafeNumeric<uint32_t> promotions;
	Ref<Resource> native;
	Ref<Resource> queried;
	Resource *owner = nullptr;
	CSharpInstance *instance = nullptr;
	String path = "res://__resource_cache_csharp_reference_race__.tres";
	Thread release_thread;
	Thread query_thread;
	bool release_started = false;
	bool query_started_successfully = false;
	bool managed_reference = false;
	GDMonoCache::ManagedCallbacks::FuncScriptManagerBridge_SwapGCHandleForType previous_swap = nullptr;

	~CSharpReferenceRaceState();
};

static CSharpReferenceRaceState *csharp_reference_race = nullptr;

static bool GD_CLR_STDCALL swap_test_gchandle(GCHandleIntPtr p_old_handle, GCHandleIntPtr *r_new_handle, bool p_create_weak) {
	CSharpReferenceRaceState &state = *csharp_reference_race;
	if (p_create_weak && state.block_weak_swap.is_set()) {
		state.weak_swap_entered.post();
		state.weak_swap_release.wait();
	} else if (!p_create_weak) {
		state.promotions.increment();
	}
	*r_new_handle = { reinterpret_cast<void *>(uintptr_t(p_create_weak ? 2 : 3)) };
	return true;
}

CSharpReferenceRaceState::~CSharpReferenceRaceState() {
	block_weak_swap.clear();
	weak_swap_release.post();
	if (release_started) {
		release_thread.wait_to_finish();
	}
	if (query_started_successfully) {
		query_thread.wait_to_finish();
	}
	if (instance) {
		// Synthetic handles must never reach real managed disposal callbacks.
		CSharpCacheTestAccess::clear_handle(instance);
		owner->set_script_instance(nullptr);
	}
	queried.unref();
	native.unref();
	if (managed_reference && owner->unreference()) {
		memdelete(owner);
	}
	GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType = previous_swap;
	csharp_reference_race = nullptr;
}

static void release_csharp_native_reference(void *p_userdata) {
	static_cast<CSharpReferenceRaceState *>(p_userdata)->native.unref();
}

static void query_csharp_resource(void *p_userdata) {
	CSharpReferenceRaceState &state = *static_cast<CSharpReferenceRaceState *>(p_userdata);
	state.query_started.post();
	state.queried = ResourceCache::get_ref(state.path);
	state.query_finished.set();
}

TEST_CASE("[ResourceCache] CSharp handle demotion excludes cached-reference preparation") {
	REQUIRE(CSharpLanguage::get_singleton() != nullptr);
	CSharpReferenceRaceState state;
	state.previous_swap = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType;
	csharp_reference_race = &state;
	GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType = swap_test_gchandle;

	state.native.instantiate();
	state.owner = state.native.ptr();
	// Stand in for the managed side's native reference without requiring a CLR.
	state.managed_reference = state.owner->reference();
	REQUIRE(state.managed_reference);
	state.instance = memnew(CSharpInstance(Ref<CSharpScript>()));
	CSharpCacheTestAccess::initialize(state.instance, state.owner);
	state.owner->set_script_instance(state.instance);
	state.owner->set_path(state.path);
	state.block_weak_swap.set();

	state.release_started = state.release_thread.start(release_csharp_native_reference, &state) != Thread::UNASSIGNED_ID;
	REQUIRE(state.release_started);
	state.weak_swap_entered.wait();
	// A temporary null handle inside a managed swap must not be observable by
	// the cache hook. This also makes the synchronization assertion independent
	// of the query worker's scheduling during the observation window below.
	CHECK_FALSE(CSharpCacheTestAccess::try_handle_lock());
	state.query_started_successfully = state.query_thread.start(query_csharp_resource, &state) != Thread::UNASSIGNED_ID;
	REQUIRE(state.query_started_successfully);
	state.query_started.wait();
	for (int i = 0; i < 100 && !state.query_finished.is_set(); i++) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK_FALSE(state.query_finished.is_set());

	state.block_weak_swap.clear();
	state.weak_swap_release.post();
	state.release_thread.wait_to_finish();
	state.release_started = false;
	state.query_thread.wait_to_finish();
	state.query_started_successfully = false;
	CHECK(state.queried.ptr() == state.owner);
	CHECK_EQ(state.promotions.get(), 1);
	CHECK(CSharpCacheTestAccess::has_strong_handle(state.instance));
}

#endif // MODULE_MONO_ENABLED

} // namespace TestResourceCache

#endif // THREADS_ENABLED
