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
// thread-local free list, if the cache grows too big
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
	while(atomic_flag_test_and_set_explicit(lock,memory_order_acquire));
}
static void spinlock_unlock(atomic_flag* lock)
{
	atomic_flag_clear_explicit(lock,memory_order_release);
}

// List of thread local reserves for the freed memory
typedef struct reserve_list {
	local_reserve* list;
	struct reserve_list* _Atomic next;
} reserve_list;

// Initializes the global list of reserves for the threads
static reserve_list* _Atomic free_lists;

// Adds a local free list to the global reserves list
static void push_free_list(reserve_list* node)
{
	while(1)
	{
		reserve_list* head = atomic_load(&free_lists);
		node->next = head;
		if(atomic_compare_exchange_strong(&free_lists, &head, node))
		{
			break;
		}
	}
}

////////// Garbage collection //////////

// Global heap for adding freed memory to so it can be collected by the garbage collector
static free_list_node* global_heap = NULL;
static atomic_flag heap_lock = ATOMIC_FLAG_INIT;	// For the spinlock

// Garbage collector initializations
static pthread_mutex_t gc_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc_cv = PTHREAD_COND_INITIALIZER;
static atomic_flag gc_init = ATOMIC_FLAG_INIT;
static atomic_size_t awakenings = ATOMIC_VAR_INIT(0);
static pthread_t garbage_collector;

// Finds the next block in the free list given a starting node
static free_list_node* next_block(free_list_node const* bl)
{
	return (free_list_node*)(((char*)bl) + bl->size);
}

// Returns true if the free list noes are adjacent to one another
static int coelescable(free_list_node const* a, free_list_node const* b)
{
	return (next_block(a) == b);
}

// Compile time constants
enum constants {
	PAGE_SIZE = 0x1000,		// Linux page size for mmap is 4096 bytes
	MIN_ALLOC_SIZE=32		// Smallest possible allocation given our structure
};

// Returns true if the given pointer is at the start of an mmap page allocation
static int page_aligned(void* addr)
{
	return (((size_t)addr) % PAGE_SIZE) == 0;
}

///// Mergesort implementation /////

// Merges free list nodes together
static free_list_node* merge_free_lists(free_list_node* a, free_list_node* b)
{
	free_list_node* ret = NULL;
	free_list_node** prev = &ret;
	while(1)
	{
		// base cases, null reached
		if(!b)
		{
			*prev = a;
			break;
		}
		if(!a)
		{
			*prev = b;
			break;
		}
		if(a->size > b->size)
		{
			*prev=a;
			prev=&a->next;
			a=a->next;
		}
		else
		{
			*prev=b;
			prev=&b->next;
			b=b->next;
		}
	}
	return ret;
}

// Sorts the free list by size using mergesort
static free_list_node* sort_free_list(free_list_node* head)
{
	// Base case, 1 or 0 elements
	if(!head || !head->next)
	{
		return head;
	}
	
	// Splits the list
	free_list_node* next = head->next;
	if(!next->next)
	{
		if(head->size < next->size)
		{
			next->next = head;
			head->next = NULL;
			return next;
		}
		else
		{
			return head;
		}
	}

	free_list_node* before_second_half = next;
	free_list_node* far = next->next;
	while(far&&(far = far->next))
	{
		far=far->next;
		before_second_half=before_second_half->next;
	}
	free_list_node* second_half = before_second_half->next;
	before_second_half->next = 0;
	second_half = sort_free_list(second_half);
	head = sort_free_list(head);

	// Linear recombination step
	return merge_free_lists(head,second_half);
}

////////// Garbage collection thread //////////

// Threaded task always running, coalesces when it can and adds memory back to the global cache
static void* cleanup(void* _)
{
	free_list_node* deleted = NULL;
	while(1)
	{
		//  Awakens the garbage collector
		pthread_mutex_lock(&gc_mtx);
		while(atomic_load_explicit(&awakenings, memory_order_acquire) == 0)
		{
			pthread_cond_wait(&gc_cv, &gc_mtx);
		}

		atomic_store_explicit(&awakenings, 0, memory_order_release);

		// For every free list in the thread reserves...
		for(reserve_list* fll = atomic_load(&free_lists); fll; fll = fll->next)
		{
			// Gathers the currently queued freelist to collect garbage from
			free_list_node* to_insert;
			// TODO: These curly braces?
			//{
				local_reserve* list = fll->list;
				spinlock_lock(&list->queue_lock);
				to_insert = list->queue;
				list->queue = 0;
				spinlock_unlock(&list->queue_lock);
			//}

			// Runs through the queued freelist and adds the memory back to the reserve
			while(to_insert)
			{
				free_list_node* next = to_insert->next;
				if(deleted)
				{
					// If the node to reinsert comes before the running deleted list head
					if(to_insert < deleted)
					{
						// Combines the memory block if adjacent, but links them regardless
						if(coelescable(to_insert,deleted))
						{
							to_insert->size += deleted->size;
							to_insert->next = deleted->next;
							deleted = to_insert;
						}
						else
						{
							to_insert->next = deleted;
						}
					}
					// if to_insert comes after the running deleted lsit head
					else
					{
						// Cycles through the deletion list and coalesces what it can before,
						// -- linking if it can't
						free_list_node* prev = deleted;
						free_list_node* head = deleted->next;
						for(; head; prev = head, head = head->next)
						{
							// Adds in the first insertable node and jumps to the end
							if(to_insert < head)
							{
								// Coalesces the insert node to the head if you can
								if(coelescable(to_insert, head))
								{
									size_t const combined_size = to_insert->size + head->size;
									if(coelescable(prev, to_insert))
									{
										prev->size += combined_size;
										prev->next = head->next;
									}
									else
									{
										to_insert->next = head->next;
										to_insert->size = combined_size;
										prev->next = to_insert;
									}
								}
								// If not, attempt with the previous node
								else if(coelescable(prev, to_insert))
								{
									prev->size += to_insert->size;
								}
								else
								{
									prev->next = to_insert;
									to_insert->next = head;
								}
								// Big brain breaks out of the loop after inserting
								goto end_loop;
							}
						}
						// If can't insert in the running head, try to coalesce or at least link
						if(coelescable(prev, to_insert))
						{
							prev->size += to_insert->size;
						}
						else
						{
							prev->next = to_insert;
							to_insert->next = head;
						}

					// End the loop after an insertion
					end_loop:;
					}
				}
				// Inserts the first node as deleted (base case)
				else
				{
					deleted = to_insert;
					deleted->next = NULL;
				}
				to_insert = next;
			}

			// TODO: Remove this, but make sure we don't need it anymore

			/*
			if(deleted)
			{
				free_list_node** prev=&deleted;
				free_list_node* head=deleted;
				do
				{
					size_t const size=deleted->size;
					size_t const num_pages=size/PAGE_SIZE;
					size_t const num_bytes=num_pages*PAGE_SIZE;
					if(num_bytes==size)
					{
						free_list_node* next=head->next;
						munmap(head,num_bytes);
						head=next;
						*prev=head;
					}
					else
					{
						free_list_node* next=head->next;
						munmap(head,num_bytes);
						free_list_node* new_head=(free_list_node*)((char*)head+num_bytes);
						new_head->next=next;
						new_head->size=size-num_bytes;
						*prev=head;
						prev=&head->next;
						head=next;
					}
				} while(head);
			}
			*/
		}

		// Updates the global heap of deleted memory 
		free_list_node* sorted = sort_free_list(deleted);
		spinlock_lock(&heap_lock);
		deleted = global_heap;
		global_heap = sorted;
		spinlock_unlock(&heap_lock);
	}

	// Need to return something when initializing the thread, even if this is never called
	return 0;
}

// Gets the thread's local free list reserve
static local_reserve* get_reserve()
{
	static __thread local_reserve fl={0, 0, 0, ATOMIC_FLAG_INIT, 0};
	static __thread reserve_list fll={0, ATOMIC_VAR_INIT((void*)0)};
	// If uninitialized
	if(unlikely(fll.list == NULL))
	{
		fll.list = &fl;
		fl.cache_end = &fl.cache;
		push_free_list(&fll);
	}
	return &fl;
}

// Byte aligns the data
static size_t fix_size(size_t _bytes)
{
	return div_up(_bytes + 16, 16)*16;
}

// Allocates memory from the global cache if there is some available
static void* take_from_cache(local_reserve* reserve, size_t const needed)
{
	// If there actually is a cache available
	if(reserve->cache)
	{
		free_list_node* el = reserve->cache;
		size_t const el_size = el->size;

		// If the needed space fits in the node, insert it here
		if(needed <= el_size)
		{
			free_list_node* next = el->next;
			memblock* ret = (memblock*)el;
			size_t const remaining = el_size - needed;

			// Splits the block and puts it back in fl if there are enough bytes
			if(remaining < MIN_ALLOC_SIZE)
			{
				reserve->cache = next;
				if(next == 0)
				{
					reserve->cache_end = &reserve->cache;
				}
			}

			// Finds space if there isn't anything available in this node
			else
			{
				free_list_node* new_node = (free_list_node*)((char*)el + needed);
				new_node->size = remaining;

				// If you've reached the end of the free list
				if(!next)
				{
					reserve->cache = new_node;
					reserve->cache_end = &new_node->next;
					new_node->next = NULL;
				}
				else
				{
					if(remaining < next->size)
					{
						(*reserve->cache_end) = new_node;
						reserve->cache = next;
					}
					else
					{
						reserve->cache = new_node;
						new_node->next = next;
					}
				}
			}

			// Returns the pointer to the new space
			ret->size = needed;
			reserve->cache_size -= needed;
			return ret->data;
		}
	}

	// Returns null if no allocable space
	return NULL;
}

// Takes from the global memory heap 
static void* take_from_global_heap(local_reserve* reserve, size_t const needed)
{
	// Locks for thread safety
	spinlock_lock(&heap_lock);
	free_list_node* head = global_heap;

	// Ensures there is enough space
	if(head && head->size >= needed)
	{
		global_heap = head->next;
		spinlock_unlock(&heap_lock);

		// If there isn't enough remaining space for another alloc, take the whole block
		size_t const remaining = head->size-needed;
		if(remaining < MIN_ALLOC_SIZE)
		{
			memblock* ret=(memblock*)head;
			return ret->data;
		}
		
		// Splits at the head if there's enough remaining for there to be another alloc
		else
		{
			// Initializes the returned memory
			memblock* ret = (memblock*)head;
			ret->size = needed;
			free_list_node* left = (free_list_node*)((char*)head + needed);
			left->size = remaining;
			reserve->cache_size += remaining;
			
			// Add the remaining space back into the reserve cache
			if(reserve->cache == NULL)
			{
				reserve->cache = left;
				left->next = NULL;
				reserve->cache_end = &left->next;
			}
			else
			{
				if(remaining < reserve->cache->size)
				{
					(*reserve->cache_end) = left;
					left->next = NULL;
					reserve->cache_end = &left->next;
				}
				else
				{
					free_list_node* next = reserve->cache;
					reserve->cache = left;
					left->next = next;
				}
			}

			// Returns the allocated pointer
			return ret->data;
		}
	}

	// Returns a null pointer if you can't take from the heap
	spinlock_unlock(&heap_lock);
	return NULL;
}

/////////////////////////
// Interface functions //
/////////////////////////

// Allocates a space of memory of the desired number of bytes and returns a pointer to it
void* xmalloc(size_t _bytes)
{
	// Asks for nothing, return nothing
	if(unlikely(!_bytes))
	{
		return 0;
	}

	// Initializes the garbage collector thread on the first malloc call
	static __thread int gc_initt = 0;
	if(unlikely(!gc_initt))
	{
		if(!atomic_flag_test_and_set(&gc_init))
		{
			pthread_create(&garbage_collector, 0, cleanup, 0);
		}
		gc_initt = 1;
	}

	// Gathers the pointers needed for the allocation and it's size and the thread's reserve
	static __thread char* data = 0;
	static __thread char* data_end = 0;
	size_t const needed = fix_size(_bytes);	// Readjusts so there's room for metadata
	local_reserve* reserve = get_reserve();
	// We will most likely take from our available cache
	{
		void* from_cache = take_from_cache(reserve, needed);
		if(from_cache)
		{
			return from_cache;
		}
	}

	// If There isn't enough data available
	if(unlikely(data + needed > data_end))
	{
		// Attempts to take from the global heap if it's available
		{
			void* from_global_heap = take_from_global_heap(reserve, needed);
			if(from_global_heap)
			{
				return from_global_heap;
			}
		}

		// If there's nothing available, we'll finally have to mmap more space
		if(data)
		{
			char* last=(char*)(div_up((size_t)data, PAGE_SIZE) * PAGE_SIZE);
			munmap(last, data_end - last);
		}
		size_t const block_size = 32 * PAGE_SIZE;
		size_t const to_alloc = block_size > needed ? block_size : needed;
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
	if(likely(ptr))
	{	
		// Gets the pointer at the start of the data with its metadata
		size_t const offset = offsetof(memblock, data);
		free_list_node* start=(free_list_node*)((char*)ptr - offset);

		// put memory on thread local cache
		// memory is more likely to be larger than previous allocations or the same size
		// so putting it on the front is a good guess
		local_reserve* reserve = get_reserve();
		size_t const size = start->size;
		if(!reserve->cache)
		{
			reserve->cache_end = &start->next;
		}
		start->next = reserve->cache;
		reserve->cache = start;
		reserve->cache_size += size;
		size_t const CACHE_LIMIT = PAGE_SIZE;
		
		// For frees of large allocations
		if(reserve->cache_size >= CACHE_LIMIT)
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
	}
	// Do nothing if freeing null
}

// Reallocates the amount of memory stored at pointer v
void* xrealloc(void* v,size_t bytes)
{
	if(likely(v))
	{
		// Copies the memory to a new malloc of the desired size and frees the old
		size_t const size = *((size_t*)v - 2);
		size_t const needed = fix_size(bytes);
		if(likely(needed > size))
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
