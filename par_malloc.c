// Challenge 2 - Fast Malloc
// Eddie Xie
// Thomas Kaunzinger
//
// Attempt 2 - Caching + Garbage Collection Method
//
// This has the Nat Tuck seal of "not actually a stupid idea."
// Can be interpreted as a simplified version of Google's TCMalloc
// Each thread allocates a block of memory to take from.
// On freeing, memory segments are consed to the front of a
// thread - local free list, if the cache grows too big
// it is sent to a garbage collector thread that will
// coealesce memory and post it to a global heap.
// On allocation, memory is taken from the free list,
// from the block of data available, from the global heap,
// or newly allocated, in that order

// Library imports
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include "xmalloc.h"

// Macros for likelihood builtins for minor comparison optimizations
// from https://www.geeksforgeeks.org/branch-prediction-macros-in-gcc/
#define likely(x)      __builtin_expect(!!(x), 1) 
#define unlikely(x)    __builtin_expect(!!(x), 0) 

//////////////////////////////////////////
// Helper functions and data structures //
//////////////////////////////////////////

// Integer division, rounding up
static size_t div_up(size_t xx, size_t yy)
{
	return (xx + yy - 1) / yy;
}

// Individual nodes marking the spaces in the free list and how much to free
typedef struct free_list_node {
	size_t size;
	struct free_list_node* next;
} free_list_node;

// A block of memory to be used
typedef struct memblock {
	size_t size;
	size_t _padding;	// Unused, keeps same alignment as freelist
	char data[];		// All the actually allocated data goes in here as bytes
} memblock;

// Cache of the local free memory in the system with some metadata - one per thread
typedef struct local_reserve {
	size_t cache_size;
	free_list_node* cache;		// Head of the freelist for this reserve
	free_list_node** cache_end;
	atomic_flag queue_lock;
	free_list_node* queue; // singly linked, how the cache is given to the garbage collector
} local_reserve;

////////// Thread locking and freelist reserves //////////

// Lock and unlocks an atomic spinlock
static void spinlock_lock(atomic_flag* lock)
{
	while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire));
}
static void spinlock_unlock(atomic_flag* lock)
{
	atomic_flag_clear_explicit(lock, memory_order_release);
}

// List of thread local reserves for the freed memory
typedef struct reserve_list {
	local_reserve* reserve;
	struct reserve_list* _Atomic next;
} reserve_list;

// Initializes the global list of reserves for the threads
static reserve_list* _Atomic free_lists;

// Adds a local free list to the global reserves list
static void push_local_reserve(reserve_list* node)
{
	while (1)
	{
		reserve_list* head = atomic_load(&free_lists);
		node->next = head;
		if (atomic_compare_exchange_strong(&free_lists, &head, node))
		{
			break;
		}
	}
}

////////// Garbage collection //////////

// Global heap for adding freed memory to so it can be collected by the garbage collector
static free_list_node* global_heap = 0;
static atomic_flag heap_lock = ATOMIC_FLAG_INIT;	// For the spinlock

// Garbage collector initializations
static pthread_mutex_t gc_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc_cv = PTHREAD_COND_INITIALIZER;
static atomic_flag gc_init = ATOMIC_FLAG_INIT;
static atomic_size_t awakenings = ATOMIC_VAR_INIT(0);
static pthread_t garbage_collector;

static free_list_node* offset_block(free_list_node const* bl, size_t offset)
{
	return (free_list_node*)(((char*)bl) + offset);
}

// Finds the next block in the free list given a starting node
static free_list_node* next_block(free_list_node const* bl)
{
	return offset_block(bl, bl->size);
}

// Returns true if the free list noes are adjacent to one another
static int coelescable(free_list_node const* a, free_list_node const* b)
{
	return next_block(a) == b;
}

// Compile time constants
enum constants {
	PAGE_SIZE = 0x1000,		// Linux page size for mmap is 4096 bytes
	MIN_ALLOC_SIZE = 32		// Smallest possible allocation given our structure
};


///// Mergesort implementation /////

typedef struct merge_result {
	free_list_node* head;
	free_list_node* last; // garbage if head is null, else ptr to last element of list
} merge_result;

// Merges two free lists together based on size
static merge_result merge_free_lists_by_size(merge_result a, merge_result b)
{
	if (a.head)
	{
		if (b.head)
		{
			// short circuit one range being completely greater than the other
			if (a.last->size >= b.head->size)
			{
				a.last->next = b.head;
				a.last = b.last;
				return a;
			}
			if (b.last->size >= a.head->size)
			{
				b.last->next = a.head;
				b.last = a.last;
				return b;
			}
		}
		else
		{
			return a;
		}
	}
	else if (b.head)
	{
		return b;
	}
	else
	{
		merge_result ret = {0, 0};
		return ret;
	}

	free_list_node head = {0, 0};
	free_list_node* prev = &head;
	free_list_node* ahead = a.head;
	free_list_node* bhead = b.head;
	while (1)
	{
		if (ahead->size > bhead->size)
		{
			prev->next = ahead;
			prev = ahead;
			ahead = ahead->next;
		}
		else
		{
			prev->next = bhead;
			prev = bhead;
			bhead = bhead->next;
		}

		// Base case reached, break and return
		if (ahead == 0)
		{
			prev->next = bhead;
			prev = b.last;
			break;
		}
		if (bhead == 0)
		{
			prev->next = ahead;
			prev = a.last;
			break;
		}
	}
	merge_result ret = {head.next,prev};
	return ret;
}

// Sorts the free list by size using mergesort
static merge_result sort_free_list_by_size(free_list_node* head)
{
	// Base case, 1 or 0 elements
	if (head == 0 || head->next == 0)
	{
		merge_result ret = {head, head};
		return ret;
	}

	// 2 element list
	free_list_node* next = head->next;
	if (next->next == 0)
	{
		if (head->size < next->size)
		{
			next->next = head;
			head->next = 0;
			merge_result ret = {next, head};
			return ret;
		}
		else
		{
			merge_result ret = {head, next};
			return ret;
		}
	}

	//split list
	free_list_node* before_second_half = next;
	free_list_node* far = next->next;
	while (far && (far = far->next))
	{
		far = far->next;
		before_second_half = before_second_half->next;
	}

	free_list_node* second_half = before_second_half->next;
	before_second_half->next = 0;
	merge_result s1 = sort_free_list_by_size(head);
	merge_result s2 = sort_free_list_by_size(second_half);
	// Linear recombination step
	return merge_free_lists_by_size(s1, s2);
}

// Merges two free lists together based on which nodes show earliest in memory
static merge_result merge_free_lists_by_address(merge_result a, merge_result b)
{
	if (a.head)
	{
		if (b.head)
		{
			// short circuit one range being completely lower than the other
			for (int i = 0; i < 2;++i)
			{
				if (a.last < b.head)
				{
					// Coalesces the blocks if possible
					if (coelescable(a.last, b.head))
					{
						a.last->size += b.head->size;
						a.last->next = b.head->next;
						if (b.head->next)
						{
							a.last = b.last;
						}
					}
					else
					{
						a.last = b.last;
					}
					return a;
				}
				merge_result temp = a;
				a = b;
				b = temp;
			}
		}
		else
		{
			return a;
		}
	}
	else if (b.head)
	{
		return b;
	}
	else
	{
		merge_result ret = {0, 0};
		return ret;
	}

	// If not at one of the heads, continue merging, coalescing where possible
	free_list_node head = {0, 0};
	free_list_node* prev = &head;
	free_list_node* ahead = a.head;
	free_list_node* bhead = b.head;
	while (1)
	{
		if (ahead < bhead)
		{
			if (coelescable(prev, ahead))
			{
				prev->size += ahead->size;
			}
			else
			{
				prev->next = ahead;
				prev = ahead;
			}
			ahead = ahead->next;
		}
		else
		{
			if (coelescable(prev, bhead))
			{
				prev->size += bhead->size;
			}
			else
			{
				prev->next = bhead;
				prev = bhead;
			}
			bhead = bhead->next;
		}
		// Base case reached, break and return
		if (ahead == 0)
		{
			if (coelescable(prev, bhead))
			{
				prev->size += bhead->size;
				prev->next = bhead->next;
			}
			else
			{
				prev->next = bhead;
			}
			if (prev->next)
			{
				prev = b.last;
			}
			break;
		}
		if (bhead == 0)
		{
			if (coelescable(prev, ahead))
			{
				prev->size += ahead->size;
				prev->next = ahead->next;
			}
			else
			{
				prev->next = ahead;
			}
			if (prev->next)
			{
				prev = a.last;
			}
			break;
		}
	}
	merge_result ret = {head.next, prev};
	return ret;
}

// Sorts the free list by memory addresses using mergesort
static merge_result sort_free_list_by_address(free_list_node* head)
{
	// Base Case, 0 or 1 element
	if (head == 0 || head->next == 0)
	{
		merge_result ret = {head, head};
		return ret;
	}

	// 2 element case
	free_list_node* next = head->next;
	if (next->next == 0)
	{
		if (head < next)
		{
			if (coelescable(head, next))
			{
				head->size += next->size;
				head->next = 0;
				merge_result ret = {head, head};
				return ret;
			}
			else
			{
				merge_result ret = {head, next};
				return ret;
			}
		}
		else
		{
			if (coelescable(next, head))
			{
				next->size += head->size;
				next->next = 0;
				merge_result ret = {next, next};
				return ret;
			}
			else
			{
				next->next = head;
				head->next = 0;
				merge_result ret = {next,head};
				return ret;
			}
		}
	}

	// Splits the list
	free_list_node* before_second_half = head;
	free_list_node* far = next->next;
	while (far && (far = far->next))
	{
		far = far->next;
		before_second_half = before_second_half->next;
	}
	free_list_node* second_half = before_second_half->next;
	before_second_half->next = 0;
	merge_result s1 = sort_free_list_by_address(second_half);
	merge_result s2 = sort_free_list_by_address(head);
	// Linear recombination step
	return merge_free_lists_by_address(s1, s2);
}

////////// Garbage collection thread //////////

// Threaded task always running, coalesces when it can and adds memory back to the global cache
static void* cleanup(void* _)
{
	merge_result deleted = {0,0};
	while (1)
	{
		//  Awakens the garbage collector
		pthread_mutex_lock(&gc_mtx);
		while (atomic_load_explicit(&awakenings, memory_order_acquire) == 0)
		{
			pthread_cond_wait(&gc_cv, &gc_mtx);
		}
		// Cleans up every free list in the thread reserves
		atomic_store_explicit(&awakenings, 0, memory_order_release);
		for (reserve_list* fll = atomic_load(&free_lists); fll; fll = fll->next)
		{
			free_list_node* to_insert;
			{
				local_reserve* reserve = fll->reserve;
				spinlock_lock(&reserve->queue_lock);
				to_insert = reserve->queue;
				reserve->queue = 0;
				spinlock_unlock(&reserve->queue_lock);
			}
			merge_result sorted_to_insert = sort_free_list_by_address(to_insert);
			deleted = merge_free_lists_by_address(sorted_to_insert, deleted);
		}

		// Updates the global heap of deleted memory with what was collected from the local threads
		if (deleted.head)
		{
			merge_result sorted = sort_free_list_by_size(deleted.head);
			spinlock_lock(&heap_lock);
			deleted.head = global_heap;
			global_heap = sorted.head;
			spinlock_unlock(&heap_lock);
			deleted = sort_free_list_by_address(deleted.head);
		}
	}

	// Need to return something when initializing the thread, even if this is never called
	return 0;
}

// Gets the thread's local free list reserve
static local_reserve* get_reserve()
{
	static __thread local_reserve reserve = {0, 0, 0, ATOMIC_FLAG_INIT, 0};
	static __thread reserve_list list = {0, ATOMIC_VAR_INIT((void*)0)};
	// If uninitialized
	if (unlikely(list.reserve == 0))
	{
		list.reserve = &reserve;
		reserve.cache_end = &reserve.cache;
		push_local_reserve(&list);
	}
	return &reserve;
}

// Byte aligns the data
static size_t fix_size(size_t _bytes)
{
	return div_up(_bytes + 16, 16) * 16;
}

// Allocates memory from the local cache if there is some available
static void* take_from_cache(local_reserve* reserve, size_t const needed)
{
	// If there actually is a cache available in this thread's reserve
	if (reserve->cache)
	{
		free_list_node* el = reserve->cache;
		size_t const el_size = el->size;

		// If the needed space fits in the node, insert it here
		if (needed <= el_size)
		{
			free_list_node* next = el->next;
			memblock* ret = (memblock*)el;
			size_t const remaining = el_size - needed;
			// Splits the block and puts it back in fl if there are enough bytes
			if (remaining < MIN_ALLOC_SIZE)
			{
				reserve->cache_size -= el_size;
				reserve->cache = next;
				if (next == 0)
				{
					reserve->cache_end = &reserve->cache;
				}
			}

			// Finds space if there isn't anything available in this node
			else
			{
				ret->size = needed;
				reserve->cache_size -= needed;
				free_list_node* new_node = offset_block(el, needed);
				new_node->size = remaining;

				// If you've reached the end of the free list
				if (next == 0)
				{
					reserve->cache = new_node;
					reserve->cache_end = &new_node->next;
					new_node->next = 0;
				}
				else
				{
					if (remaining < next->size)
					{
						(*reserve->cache_end) = new_node;
						reserve->cache = next;
						reserve->cache_end = &new_node->next;
					}
					else
					{
						reserve->cache = new_node;
						new_node->next = next;
					}
				}
			}

			size_t const CACHE_LIMIT = 20 * PAGE_SIZE;

			// For frees of large allocations
			if (reserve->cache_size >= CACHE_LIMIT)
			{
				spinlock_lock(&reserve->queue_lock);
				(*reserve->cache_end) = reserve->queue;
				reserve->queue = reserve->cache;
				spinlock_unlock(&reserve->queue_lock);
				// Awakens the garbage collector thread
				atomic_fetch_add_explicit(&awakenings, 1, memory_order_release);
				pthread_cond_signal(&gc_cv);
				reserve->cache = 0;
				reserve->cache_end = &reserve->cache;
				reserve->cache_size = 0;
			}

			return ret->data;
		}
	}

	// Returns null if no allocable space
	return 0;
}

// Inserts a node into this local thread's reserved cache
static void insert_into_cache(local_reserve* reserve, free_list_node* node, size_t const block_size)
{
	reserve->cache_size += block_size;
	if (reserve->cache == 0)
	{
		reserve->cache = node;
		node->next = 0;
		reserve->cache_end = &node->next;
	}
	else
	{
		if (block_size < reserve->cache->size)
		{
			(*reserve->cache_end) = node;
			node->next = 0;
			reserve->cache_end = &node->next;
		}
		else
		{
			free_list_node* next = reserve->cache;
			reserve->cache = node;
			node->next = next;
		}
	}
}

// Takes from the global memory heap 
static void* take_from_global_heap(local_reserve* reserve, size_t const needed)
{
	// Locks for thread safety
	spinlock_lock(&heap_lock);
	free_list_node* head = global_heap;

	// Ensures there is enough space
	if (head && head->size >= needed)
	{
		global_heap = head->next;
		spinlock_unlock(&heap_lock);

		// If there isn't enough remaining space for another alloc, take the whole block
		size_t const remaining = head->size - needed;
		if (remaining < MIN_ALLOC_SIZE)
		{
			memblock* ret = (memblock*)head;
			return ret->data;
		}

		// Splits at the head if there's enough remaining for there to be another alloc
		else
		{
			// Initializes the returned memory
			memblock* ret = (memblock*)head;
			ret->size = needed;
			free_list_node* left = offset_block(head, needed);
			left->size = remaining;
			insert_into_cache(reserve, left, remaining);
			return ret->data;
		}
	}

	// Returns a null pointer if you can't take from the heap
	spinlock_unlock(&heap_lock);
	return 0;
}

/////////////////////////
// Interface functions //
/////////////////////////

// Allocates a space of memory of the desired number of bytes and returns a pointer to it
void* xmalloc(size_t _bytes)
{
	// Asks for nothing, return nothing
	if (unlikely(_bytes == 0))
	{
		return 0;
	}

	// Initializes the garbage collector thread on the first malloc call
	static __thread int gc_inited = 0;
	if (unlikely(!gc_inited))
	{
		if (!atomic_flag_test_and_set(&gc_init))
		{
			pthread_create(&garbage_collector, 0, cleanup, 0);
		}
		gc_inited = 1;
	}

	// Gathers the pointers needed for the allocation and it's size and the thread's reserve
	static __thread char* data = 0;
	static __thread char* data_end = 0;
	size_t const needed = fix_size(_bytes);	// Readjusts so there's room for metadata
	local_reserve* reserve = get_reserve();
	// We will most likely take from our available cache
	{
		void* from_cache = take_from_cache(reserve, needed);
		if (from_cache)
		{
			return from_cache;
		}
	}

	// If There isn't enough data available
	if (unlikely(data + needed > data_end))
	{
		// Attempts to take from the global heap if it's available
		{
			void* from_global_heap = take_from_global_heap(reserve, needed);
			if (from_global_heap)
			{
				return from_global_heap;
			}
		}

		// If there's nothing available, we'll finally have to mmap more space
		if (data)
		{
			char* last = (char*)(div_up((size_t)data, PAGE_SIZE) * PAGE_SIZE);
			munmap(last, data_end - last);
		}
		size_t const block_size = 16 * PAGE_SIZE;
		size_t const to_alloc = block_size > needed
			? block_size : (div_up(needed, PAGE_SIZE) * PAGE_SIZE);
		data = mmap(0, to_alloc, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		data_end = data + to_alloc;
	}

	// Reutrns the data that's safe to use
	memblock* ret = (memblock*)data;
	ret->size = needed;
	data += needed;
	return ret->data;
}

// Frees the memory back into the system that can be reused later
void xfree(void* ptr)
{
	if (likely(ptr))
	{
		// Gets the pointer at the start of the data with its metadata
		size_t const offset = offsetof(memblock, data);
		free_list_node* start = (free_list_node*)((char*)ptr - offset);

		// put memory on thread local cache
		// memory is more likely to be larger than previous allocations or the same size
		// so putting it on the front is a good guess
		local_reserve* reserve = get_reserve();
		size_t const size = start->size;
		insert_into_cache(reserve, start, size);
	}
	// Do nothing if freeing null
}

// Reallocates the amount of memory stored at pointer v
void* xrealloc(void* v, size_t bytes)
{
	if (likely(v))
	{
		// Copies the memory to a new malloc of the desired size and frees the old
		size_t const size = *((size_t*)v - 2);
		size_t const needed = fix_size(bytes);
		if (likely(needed > size))
		{
			void* ret = xmalloc(bytes);
			memcpy(ret, v, size - 16);
			xfree(v);
			return ret;
		}
		// Don't even bother if you're allocating less
		return v;
	}
	// If null, just do a normal malloc
	else
	{
		return xmalloc(bytes);
	}
}
