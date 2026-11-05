/**************************************************************************/
/*  test_resource_cache.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                     */
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
	// The script destructor has begun but its storage remains behind the gate.
	// Neither cache path may call its preparation hook at this point.
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

TEST_CASE("[ResourceCache] Predelete cannot destroy a script while cache preparation holds the lock") {
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
	// Give the already-started cleanup a bounded chance to expose the old
	// ordering; the resource cache lock must prevent it reaching the script.
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

TEST_CASE("[ResourceCache] An uncached resource can retire while cache preparation holds the lock") {
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
	// This object never entered the cache. Its script can be destroyed while
	// another resource's preparation callback still owns the cache lock.
	bool destructor_started_without_cache = false;
	for (int i = 0; i < 100; i++) {
		if (uncached.destructor_entered.try_wait()) {
			destructor_started_without_cache = true;
			break;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(destructor_started_without_cache);
	// Release the cache even on failure, so both workers can always be joined.
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

} // namespace TestResourceCache

#endif // THREADS_ENABLED
