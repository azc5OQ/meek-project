//
// clib/memory_manager.c - allocation tracker implementation.
//
// Layout of one allocation on the heap:
//
//     [ _mm_header | user bytes ... ]
//       ^            ^
//       real pointer returned by malloc    pointer returned to caller
//
// memory_manager__alloc calls malloc for (sizeof(header) + size),
// fills the header, links it into a global doubly-linked list, and
// returns (header + 1). memory_manager__free receives the user
// pointer, steps back to the header, unlinks, and frees the original
// allocation. Both paths are O(1) -- no table scan, no hashing.
//
// The guard field catches a few common bugs cheaply:
//   * free'ing a pointer that wasn't allocated through this tracker
//     (guard won't match)
//   * double-free (second free sees a scrubbed or re-used guard)
//   * overwriting the header from underrun writes off the user block
//
// THREAD SAFETY
// -------------
// A single mutex protects the live-list head and the stats struct.
// Everything touching either side (alloc, realloc, free, get_stats,
// dump_live) takes the mutex for the duration of the list/stats
// update. The underlying malloc/realloc/free calls are thread-safe
// on every C runtime we target, but we still need to serialize
// "allocate + link + bump stats" as one atomic step so no concurrent
// observer sees a half-updated list.
//

#include "memory_manager.h"
#include "../third_party/log.h"
#include "../types.h"

#include <stdlib.h>
#include <string.h>

//
// Platform-specific mutex. CRITICAL_SECTION on Windows (recursive by
// default, cheap uncontended lock in user space), pthread_mutex_t on
// Android / Linux / macOS. Wrapped behind a tiny helper API so the
// public functions below don't have to #ifdef at every call site.
//
#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION _memory_manager_internal__mutex;
static void _memory_manager_internal__mutex_init(_memory_manager_internal__mutex *m)
{
	InitializeCriticalSection(m);
}

static void _memory_manager_internal__mutex_destroy(_memory_manager_internal__mutex *m)
{
	DeleteCriticalSection(m);
}

static void _memory_manager_internal__mutex_lock(_memory_manager_internal__mutex *m)
{
	EnterCriticalSection(m);
}

static void _memory_manager_internal__mutex_unlock(_memory_manager_internal__mutex *m)
{
	LeaveCriticalSection(m);
}

#else
#include <pthread.h>
typedef pthread_mutex_t _memory_manager_internal__mutex;
static void _memory_manager_internal__mutex_init(_memory_manager_internal__mutex *m)
{
	pthread_mutex_init(m, NULL);
}

static void _memory_manager_internal__mutex_destroy(_memory_manager_internal__mutex *m)
{
	pthread_mutex_destroy(m);
}

static void _memory_manager_internal__mutex_lock(_memory_manager_internal__mutex *m)
{
	pthread_mutex_lock(m);
}

static void _memory_manager_internal__mutex_unlock(_memory_manager_internal__mutex *m)
{
	pthread_mutex_unlock(m);
}

#endif

//
// Inline header. prev/next thread all live allocations into a
// doubly-linked list rooted at _memory_manager_internal__head so
// dump_live can walk them and free can unlink in O(1).
//
#define _MEMORY_MANAGER_INTERNAL__GUARD 0xA110C0DEu
#define _MEMORY_MANAGER_INTERNAL__FREED 0xDEADBEEFu

typedef struct _memory_manager_internal__header
{
	struct _memory_manager_internal__header *prev;
	struct _memory_manager_internal__header *next;
	size_t size;
	mm_type type;
	uint guard;
} _memory_manager_internal__header;

//
// File-local state. Every public function takes _lock before touching
// _head or _stats. init() creates the mutex; shutdown() destroys it.
// Callers must call init() before any GUI_MALLOC / GUI_FREE.
//
static _memory_manager_internal__header *_memory_manager_internal__head = NULL;
static memory_manager_stats _memory_manager_internal__stats;
static _memory_manager_internal__mutex _memory_manager_internal__lock;
static boole _memory_manager_internal__initialized = FALSE;

static const char *_memory_manager_internal__type_names[MM_TYPE_COUNT] = {
	"GENERIC",
	"TEXT",
	"NODE",
	"STYLE",
	"FONT",
	"IMAGE",
	"RENDERER",
	"FS",
};

//
// Steps from a user pointer back to its header. No validation here;
// callers that need to reject foreign pointers check the guard after.
//
static _memory_manager_internal__header *_memory_manager_internal__header_of(void *user_ptr)
{
	return ((_memory_manager_internal__header *)user_ptr) - 1;
}

//
// List helpers. Must be called with the mutex held.
//
static void _memory_manager_internal__link(_memory_manager_internal__header *h)
{
	h->prev = NULL;
	h->next = _memory_manager_internal__head;
	if (_memory_manager_internal__head != NULL)
	{
		_memory_manager_internal__head->prev = h;
	}
	_memory_manager_internal__head = h;
}

static void _memory_manager_internal__unlink(_memory_manager_internal__header *h)
{
	if (h->prev != NULL)
	{
		h->prev->next = h->next;
	}
	else
	{
		_memory_manager_internal__head = h->next;
	}
	if (h->next != NULL)
	{
		h->next->prev = h->prev;
	}
	h->prev = NULL;
	h->next = NULL;
}

void memory_manager__init(void)
{
	if (_memory_manager_internal__initialized)
	{
		return;
	}
	_memory_manager_internal__mutex_init(&_memory_manager_internal__lock);
	_memory_manager_internal__head = NULL;
	memset(&_memory_manager_internal__stats, 0, sizeof(_memory_manager_internal__stats));
	_memory_manager_internal__initialized = TRUE;
}

void memory_manager__shutdown(void)
{
	if (!_memory_manager_internal__initialized)
	{
		return;
	}

	_memory_manager_internal__mutex_lock(&_memory_manager_internal__lock);
	memory_manager_stats s = _memory_manager_internal__stats;
	_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);

	if (s.live_allocs != 0 || s.live_bytes != 0)
	{
		log_warn("memory_manager: %lld leaked allocation(s), %lld bytes", (long long)s.live_allocs, (long long)s.live_bytes);
		memory_manager__dump_live();
	}
	else
	{
		log_info("memory_manager: no leaks. %lld allocs / %lld frees, peak %lld bytes", (long long)s.total_allocs, (long long)s.total_frees, (long long)s.peak_bytes);
	}

	_memory_manager_internal__mutex_destroy(&_memory_manager_internal__lock);
	_memory_manager_internal__initialized = FALSE;
}

void *memory_manager__alloc(size_t size, mm_type type)
{
	if (size == 0)
	{
		return NULL;
	}

	//
	// malloc itself is thread-safe on every supported C runtime, so
	// do it outside the lock to minimize time spent holding the
	// mutex. Only the list + stats mutations need serialization.
	//
	_memory_manager_internal__header *h = (_memory_manager_internal__header *)malloc(sizeof(_memory_manager_internal__header) + size);
	if (h == NULL)
	{
		return NULL;
	}

	h->size = size;
	h->type = (type < MM_TYPE_COUNT) ? type : MM_TYPE_GENERIC;
	h->guard = _MEMORY_MANAGER_INTERNAL__GUARD;

	_memory_manager_internal__mutex_lock(&_memory_manager_internal__lock);

	_memory_manager_internal__link(h);
	_memory_manager_internal__stats.live_allocs += 1;
	_memory_manager_internal__stats.total_allocs += 1;
	_memory_manager_internal__stats.live_bytes += (int64)size;
	_memory_manager_internal__stats.live_bytes_by_type[h->type] += (int64)size;
	if (_memory_manager_internal__stats.live_bytes > _memory_manager_internal__stats.peak_bytes)
	{
		_memory_manager_internal__stats.peak_bytes = _memory_manager_internal__stats.live_bytes;
	}

	_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);

	return (void *)(h + 1);
}

void *memory_manager__calloc(size_t count, size_t size, mm_type type)
{
	//
	// Multiply-overflow guard. Standard calloc rejects this; we
	// need to too. count * size wrapping past SIZE_MAX would yield
	// a tiny allocation while the caller expected count*size bytes,
	// and the next memset would write past the end of the heap
	// block. Adversarial caller can drive this; even a benign
	// count=1 size=very-large gets caught.
	//
	if (count != 0 && size > (size_t)-1 / count)
	{
		return NULL;
	}
	size_t total = count * size;
	//
	// alloc() takes the lock for us. memset happens outside the lock;
	// the block isn't reachable from any other thread yet (we just
	// handed it out), so zeroing it unsynchronized is safe.
	//
	void *p = memory_manager__alloc(total, type);
	if (p != NULL)
	{
		memset(p, 0, total);
	}
	return p;
}

void *memory_manager__realloc(void *ptr, size_t new_size)
{
	if (ptr == NULL)
	{
		return memory_manager__alloc(new_size, MM_TYPE_GENERIC);
	}
	if (new_size == 0)
	{
		memory_manager__free(ptr);
		return NULL;
	}

	_memory_manager_internal__header *old_h = _memory_manager_internal__header_of(ptr);

	//
	// Guard check INSIDE the lock for the same reason memory_manager__free
	// moves it inside: two concurrent reallocs on the same block could
	// both pass an unsynchronized guard read and double-realloc.
	//
	_memory_manager_internal__mutex_lock(&_memory_manager_internal__lock);

	if (old_h->guard != _MEMORY_MANAGER_INTERNAL__GUARD)
	{
		_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);
		log_error("memory_manager__realloc: bad guard on %p (not a tracked block, "
				  "or corrupted)",
			ptr);
		return NULL;
	}

	size_t old_size = old_h->size;
	mm_type type = old_h->type;

	_memory_manager_internal__header *prev = old_h->prev;
	_memory_manager_internal__header *next = old_h->next;
	_memory_manager_internal__unlink(old_h);

	_memory_manager_internal__header *new_h = (_memory_manager_internal__header *)realloc(old_h, sizeof(_memory_manager_internal__header) + new_size);
	if (new_h == NULL)
	{
		//
		// realloc failed; the original block is still valid per the
		// C standard. Re-link it so we don't lose track of it.
		//
		old_h->prev = prev;
		old_h->next = next;
		if (prev != NULL)
		{
			prev->next = old_h;
		}
		else
		{
			_memory_manager_internal__head = old_h;
		}
		if (next != NULL)
		{
			next->prev = old_h;
		}
		_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);
		return NULL;
	}

	new_h->prev = prev;
	new_h->next = next;
	new_h->size = new_size;
	new_h->type = type;
	new_h->guard = _MEMORY_MANAGER_INTERNAL__GUARD;
	if (prev != NULL)
	{
		prev->next = new_h;
	}
	else
	{
		_memory_manager_internal__head = new_h;
	}
	if (next != NULL)
	{
		next->prev = new_h;
	}

	int64 delta = (int64)new_size - (int64)old_size;
	_memory_manager_internal__stats.live_bytes += delta;
	_memory_manager_internal__stats.live_bytes_by_type[type] += delta;
	if (_memory_manager_internal__stats.live_bytes > _memory_manager_internal__stats.peak_bytes)
	{
		_memory_manager_internal__stats.peak_bytes = _memory_manager_internal__stats.live_bytes;
	}

	_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);

	return (void *)(new_h + 1);
}

void memory_manager__free(void *ptr)
{
	if (ptr == NULL)
	{
		return;
	}

	_memory_manager_internal__header *h = _memory_manager_internal__header_of(ptr);

	//
	// Guard check is INSIDE the lock so two threads concurrently
	// freeing the same pointer can't both pass the check. Without
	// the lock around the read+write of the guard, two threads
	// could both see GUARD, both pass the gate, both unlink + free
	// -- silent double-free that the FREED-marker check is supposed
	// to catch but can't because neither thread had written it yet
	// when the other read.
	//
	_memory_manager_internal__mutex_lock(&_memory_manager_internal__lock);

	if (h->guard == _MEMORY_MANAGER_INTERNAL__FREED)
	{
		_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);
		log_error("memory_manager__free: double-free detected on %p", ptr);
		return;
	}
	if (h->guard != _MEMORY_MANAGER_INTERNAL__GUARD)
	{
		_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);
		log_error("memory_manager__free: bad guard on %p (not a tracked block, or "
				  "corrupted header)",
			ptr);
		return;
	}

	_memory_manager_internal__stats.live_allocs -= 1;
	_memory_manager_internal__stats.total_frees += 1;
	_memory_manager_internal__stats.live_bytes -= (int64)h->size;
	_memory_manager_internal__stats.live_bytes_by_type[h->type] -= (int64)h->size;

	_memory_manager_internal__unlink(h);
	h->guard = _MEMORY_MANAGER_INTERNAL__FREED;

	_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);

	//
	// free() itself is thread-safe; no need to hold our lock for it.
	//
	free(h);
}

memory_manager_stats memory_manager__get_stats(void)
{
	memory_manager_stats snapshot;
	_memory_manager_internal__mutex_lock(&_memory_manager_internal__lock);
	snapshot = _memory_manager_internal__stats;
	_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);
	return snapshot;
}

const char *memory_manager__type_name(mm_type type)
{
	if ((int)type < 0 || type >= MM_TYPE_COUNT)
	{
		return "?";
	}
	return _memory_manager_internal__type_names[type];
}

void memory_manager__dump_live(void)
{
	//
	// Everything that reads _head or _stats runs under the lock.
	// log_* calls inside the critical section serialize against each
	// other (rxi-log has its own internal lock), but that's already
	// how the rest of the project uses the logger.
	//
	_memory_manager_internal__mutex_lock(&_memory_manager_internal__lock);

	memory_manager_stats s = _memory_manager_internal__stats;
	log_info("memory_manager: live=%lld bytes across %lld allocs (peak %lld bytes)", (long long)s.live_bytes, (long long)s.live_allocs, (long long)s.peak_bytes);

	for (int i = 0; i < MM_TYPE_COUNT; i++)
	{
		if (s.live_bytes_by_type[i] != 0)
		{
			log_info("  %-9s %lld bytes", memory_manager__type_name((mm_type)i), (long long)s.live_bytes_by_type[i]);
		}
	}

	int shown = 0;
	for (_memory_manager_internal__header *h = _memory_manager_internal__head; h != NULL; h = h->next)
	{
		if (shown < 32)
		{
			log_info("  [%s] %zu bytes @ %p", memory_manager__type_name(h->type), h->size, (void *)(h + 1));
		}
		shown++;
	}
	if (shown > 32)
	{
		log_info("  ... and %d more", shown - 32);
	}

	_memory_manager_internal__mutex_unlock(&_memory_manager_internal__lock);
}
