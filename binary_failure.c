// Challenge 2 - Fast Malloc
// Eddie Xie
// Thomas Kaunzinger

// Library imports
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "xmalloc.h"

/////////////////////////
// Data and Structures //
/////////////////////////

// Buckets for buddy system
// -- Currently uses powers of 2 and intermediate values
static uint16_t const class_sizes[]={
	0x0020,		// 0 - 32 bits		(minimum allocation)
	0x0030,		// 1 - 48 bits
	0x0040,		// 2 - 64 bits
	0x0060,		// 3 - 96 bits
	0x0080,		// 4 - 128 bits
	0x00C0,		// 5 - 192 bits
	0x0100,		// 6 - 256 bits
	0x0180,		// 7 - 384 bits
	0x0200,		// 8 - 512 bits
	0x0300,		// 9 - 768 bits
	0x0400,		// 10 - 1024 bits
	0x0600,		// 11 - 1536 bits
	0x0800,		// 12 - 2048 bits
	0x0C00,		// 13 - 3072 bits	(not possible)
	0x1000		// 14 - 4096 bits	(page size)
};

// Gets the size of the bucket given a desired index
static size_t const get_class_size(size_t index)
{	// Odd indicies give equivalent of even indicies plus half of self
	if(index & 1)
	{
		size_t const b = ((size_t)1)<<(index/2 + 4);
		return b + b / 2;
	}
	return ((size_t)1)<<(index/2+4);
}

// Compile time constants
enum constants {
	CLASS_SIZE_COUNT = 15,	// Number of buckets in buddy allocator
	ARENA_COUNT = 8			// Number of arenas for the 
};

// Page size - switch from buddy allocation to page allocation
static size_t const BIG_LIMIT=0x1000;

// Block of memory when actually in use
typedef struct memblock {
	union {					// TODO: Eddie why is this a union?
		// Packed data struct for header information
		struct {
			size_t arena_source:16;
			size_t:16;
			size_t size_index:16;
			size_t:14;
			size_t left:1;
			size_t used:1;	// Shows that this mem block is still used
		};
	};
	// Remaining bytes go in this buffer
	char data[];
} memblock;

// Nodes for the free list for each bucket - Doubly linked list
typedef struct free_list {
	// Can link to either other free list or used data
	union {
		struct free_list* prev;
		struct {
			size_t:60;
			size_t:2;
			size_t left:1;	// last 4 bits of pointers are always 0
			size_t used:1;	// -- can use to encode relevant flags
		};
	};
	union {
		struct free_list* next;
		struct {
			size_t:60;
			size_t index:4;
		};
	};
} free_list;

//////////////////////
// Helper functions //
//////////////////////

// Gets the current bucket index of the free list node using the encoded 4 bytes
static size_t fl_get_index(free_list const* fl)
{
	return fl->index;
}

// Updates the free list node's encoded bucket index
static void fl_set_index(free_list* fl,size_t index)
{
	fl->index = index;
}

////////// Helpers for linking free list nodes //////////

// Mask for various bitwise operations on the free list nodes
enum fl_masks {
	FL_PREV_MASK = (((size_t)-1)<<4)
};

// Gets and sets the previous node in the free list, accounting for masked data
static free_list* fl_get_prev(free_list* fl)
{
	return (free_list*)((size_t)fl->prev&FL_PREV_MASK);
}
static void fl_set_prev(free_list* fl,free_list* prev)
{
	// TODO: Eddie wtf is this operation
	return fl->prev=(free_list*)(((size_t)fl->prev&~FL_PREV_MASK)|((size_t)prev));
}

// Gets and sets the next node in the free list, accounting for masked data
static free_list* fl_get_next(free_list* fl)
{
	return (free_list*)((size_t)fl->next&FL_PREV_MASK);
}
static void fl_set_next(free_list* fl,free_list* next)
{
	// TODO: This one too
	return fl->next=(free_list*)(((size_t)fl->next&~FL_PREV_MASK)|((size_t)next));
}

////////// Helpers for thread locking and unlocking operations //////////

// Lock and and unlock an atomic spinlock
static void spinlock_lock(atomic_flag* flag)
{
	while(atomic_flag_test_and_set(flag));
}
static void spinlock_unlock(atomic_flag* flag)
{
	atomic_flag_clear(flag);
}

////////// Arena setup ///////////

// Defines the arena
typedef struct arena {
	atomic_flag lock;	// Flag to ensure no collisions in multiple processes
	free_list* buckets[CLASS_SIZE_COUNT];	// Buddy system buckets!
} arena;

static size_t _Atomic current_arena = 0;

// Using 8 arenas by default to avoid most collisions and needs to lock
static arena arenas[ARENA_COUNT];

////////// Other helpers ///////////

// Calculates log2 of x fast using buitin leading zeros
static size_t log_2(size_t x)
{
	return 63-__builtin_clzll(x);
}

// Calculates the index of the bucket array that would have the bucket that best fits this size
static size_t class_index(size_t _bytes)
{
	size_t const bytes = _bytes+8;
	// Uses the smallest bucket if needed
	if(bytes < class_sizes[0])		// TODO: Fixed a hardcoded size, but I'll bet there's more
	{
		return 0;
	}

	// Uses our log function and masking to calculate which index is most appropriate
	size_t const l2 = log_2(bytes-1);
	size_t const base = (l2-4)*2;
	size_t const mask = (((size_t)1)<<(l2-1));
	size_t const offset = bytes&mask;
	return base + 1 + !!offset;
}

// Integer division, rounding up
static size_t div_up(size_t xx,size_t yy)
{
	return (xx+yy-1)/yy;
}

// TODO: This uhhh isn't actually used - there are probably others 
static size_t floor2(size_t x)
{
	return ((size_t)1)<<log_2(x);
}

// Gets this thread's arena number using an atomic fetch/add to a global counter
static size_t get_arena_number()
{
	static __thread size_t arena_number = -1;
	if(arena_number == -1)
	{
		arena_number = atomic_fetch_add_explicit(&current_arena,1,memory_order_relaxed)%ARENA_COUNT;
	}
	return arena_number;
}

// Inserts a used memory block in the front of a specified bucket's freelist in a given arena
static void insert_block_at_front(memblock* bl, arena* ar, size_t class_index)
{
	// Gathers the head at the given bucket and swaps with the new block as a free list node
	free_list* head = ar->buckets[class_index];
	free_list* new_head = (free_list*)bl;
	ar->buckets[class_index] = new_head;
	// Updates the new freelist head properties
	new_head->used = 0;
	fl_set_index(new_head, class_index);
	fl_set_next(new_head, head);
	fl_set_prev(new_head, NULL);
	if(head)
	{
		fl_set_prev(head, new_head);
	}	// TODO: Shouldn't we coalesce if else?
}

// TODO: Eddie what is this for we never use it? 
static void remove_block(free_list* bl, arena* ar, size_t class_index)
{
	free_list* prev=fl_get_prev(bl);
	free_list* next=fl_get_next(bl);
	if(prev)
	{
		fl_set_next(prev,next);
	}
	else
	{
		ar->buckets[class_index]=next;
	}
	if(next)
	{
		fl_set_prev(next,prev);
	}
}

// Splits a free block in the free list to insert the newly allocated memory and subsequently save
// -- the excess free space into a lower level in the buckets of the free list
static memblock* split_block_insert(size_t arena_number, size_t block_class_index,
		memblock* block, size_t needed_index)
{
	// Working in this thread's arena only
	arena* arena = arenas + arena_number + arena_number;

	// TODO: Eddie what is this variable??? 
	if((needed_index-block_class_index) & 1)
	{
		while(1)
		{	
			// If this bucket isn't big enough	
			if(needed_index + 1 >= block_class_index)
			{
				break;
			}
			// If this bucket is sufficient
			else
			{	
				// Splitting will bring you to two indicies down given our current bucket config
				size_t const split_index = block_class_index-2;

				// Splits the mem block to the right ? (TODO: right?)
				memblock* right_part = (memblock*)((char*)block+class_sizes[split_index]);
				right_part->left = 0;
				// Inserts the remaining space to the front as free
				insert_block_at_front(right_part,arena,split_index);
				block_class_index = split_index;
			}
		}
	}
	else
	{
		while(1)
		{
			if(needed_index == block_class_index)
			{
				break;
			}
			else
			{
				// Splitting will bring you to two indicies down given our current bucket config
				size_t const split_index = block_class_index-2;

				// Splits the mem block to the right ? (TODO: right?)
				memblock* right_part = (memblock*)((char*)block+class_sizes[split_index]);
				right_part->left = 0;
				// Inserts the remaining space to the front as free
				insert_block_at_front(right_part,arena,split_index);
				block_class_index = split_index;
				// TODO: Eddie, can you explain to me what the "right_part" is for?
			}
		}
	}

	// Configures and returns the newly added block
	block->size_index = needed_index;
	block->used = 1;
	block->left = 1;
	block->arena_source = arena_number;
	return block;
}

/////////////////////////
// Interface functions //
/////////////////////////

// Returns a pointer with at least "_bytes" amount of memory available to work with safely
void* xmalloc(size_t const _bytes)
{
	// Alocates exactly 0 bytes
	if(_bytes == 0)
	{
		return NULL;
	}

	// Figures our which bucket this number of bytes needs to be in
	size_t const index = class_index(_bytes);

	// If it's too big for any individual bucket, give it its own page(s)
	if(index >= CLASS_SIZE_COUNT)
	{
		// Maps the system memory
		memblock* ret = mmap(0, (size_t)1<<(index/2), PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		ret->used = 1;
		ret->size_index = index;
		return ret->data;
	}

	// Else: put the memory somewhere in our buddy free list if there's room for it
	size_t const arena_number=get_arena_number();
	spinlock_lock(&arenas[arena_number].lock);	// Ensures thread safety if colliding
	for(size_t i = index; i < CLASS_SIZE_COUNT; ++i)
	{
		// If there's a free list chunk available starting from the smallest possible bucket
		free_list* chunk = arenas[arena_number].buckets[i];
		if(chunk)
		{
			size_t const class_size = class_sizes[i];	// TODO: Unused?
			free_list* next = fl_get_next(chunk);
			if(next)	// If there is a neighbor in the FL, make sure they know
			{
				fl_set_prev(next, 0);
			}
			arenas[arena_number].buckets[i] = fl_get_next(chunk);

			// Split's this bucket's memory to fit this piece and leave some space left over
			memblock* ret = split_block_insert(arena_number, i, chunk, index);
			return ret->data;
		}
		// TODO: Maybe don't even go into the above branch but another one if the index happens to
		// -- be exactly the same and don't even bother worrying about splits for that case?
	}

	// Last resort - mmaps if there's no space in the buckets
	// TODO - What if we allocated like waaaay more than a page here when it happened to reduce
	// -- the number of system calls? Obviously not like a GB, but like maybe several MB?
	memblock* block = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	memblock* ret = split_block_insert(arena_number, class_index(4096), block, index);

	// TODO: Eddie you uhh forgot this, that's prolly why we were deadlocking. It can prolly go
	// -- somewhere better than this.
	spinlock_unlock(&arenas[arena_number].lock);
	
	// Returns the void pointer to the usable memory
	return (void*)ret->data;
}

// Frees the memory at the pointer back to the free list to reallocate.
// TODO: Eddie we don't technically need this I'm pretty sure and I don't think it will affect the
// -- tests, (in fact, it should make them faster), so consider that if we're on a time crunch.
void xfree(void* ptr)
{
	if(ptr)	// If not a NULL pointer
	{
		// Offsets to get the hidden header data for the pointer
		memblock* start = (memblock*)((char*)ptr-offsetof(memblock, data));
		size_t index = start->size_index;

		// Just unmaps if it's a big boi page
		if(index >= CLASS_SIZE_COUNT)
		{
			munmap(start,get_class_size(index));
		}

		// Else, adds to the appropriate bucket's running free list
		else
		{
			size_t const arena_number = start->arena_source;
			spinlock_lock(&arenas[arena_number].lock);		// Locks for thread safety
			free_list* to_insert = (free_list*)start;
			while(index<CLASS_SIZE_COUNT)
			{
				if(to_insert)	// TODO: I assume you didn't mean to_in?
				{

					// TODO: Tbh wtf is going on in this whole section?

					free_list* buddy = (free_list*)((char*)start + get_class_size(index));
					if(!buddy->used && fl_get_index(buddy) == start->buddy_size_index)
					{
						
					}
					else
					{
						insert_block_at_front(start, arenas + arena_number, index);
					}
				}
				else
				{
					free_list* buddy = (free_list*)((char*)start -
							get_class_size(start->buddy_size_index));
					if(!buddy->used && fl_get_index(buddy) == start->buddy_size_index)
					{

					}
					else
					{
						insert_block_at_front(start, arenas + arena_number, index);
					}
				}
			}
			insert_block_at_front((memblock*)to_insert, arenas + arena_number, index);
			spinlock_unlock(&arenas[arena_number].lock);
		}
	}
}

// Re-allocates the memory of a pointer to a new specified size in memory
void* xrealloc(void* prev, size_t bytes)
{
	if(prev)
	{
		memblock* start = (memblock*)((char*)prev - offsetof(memblock, data));
		size_t const size = class_sizes[start->size_index] - 8;
		if(size < bytes)
		{
			return prev;
		}
		else
		{
			void* ret = xmalloc(bytes);
			memcpy(ret, prev, size);
			xfree(prev);
			return ret;
		}
	}
	return 0;
}

