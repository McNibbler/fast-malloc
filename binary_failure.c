#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "xmalloc.h"

static uint16_t const class_sizes[]={
	0x0020,
	0x0030,
	0x0040,
	0x0060,
	0x0080,
	0x00C0,
	0x0100,
	0x0180,
	0x0200,
	0x0300,
	0x0400,
	0x0600,
	0x0800,
	0x0C00,
	0x1000
};

static size_t const get_class_size(size_t index)
{
	if(index&1)
	{
		size_t const b=((size_t)1)<<(index/2+4);
		return b+b/2;
	}
	return ((size_t)1)<<(index/2+4);
}

enum constants {
	CLASS_SIZE_COUNT=15,
	ARENA_COUNT=8
};

static size_t const BIG_LIMIT=0x1000;

typedef struct memblock {
	union {
		struct {
			size_t arena_source:16;
			size_t:16;
			size_t size_index:16;
			size_t:14;
			size_t left:1;
			size_t used:1;
		};
	};
	char data[];
} memblock;

typedef struct free_list {
	union {
		struct free_list* prev;
		struct {
			size_t:60;
			size_t:2;
			size_t left:1;
			size_t used:1;
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

static size_t fl_get_index(free_list const* fl)
{
	return fl->index;
}

static void fl_set_index(free_list* fl,size_t index)
{
	fl->index=index;
}

enum fl_masks {
	FL_PREV_MASK=(((size_t)-1)<<4)
};

static free_list* fl_get_prev(free_list* fl)
{
	return (free_list*)((size_t)fl->prev&FL_PREV_MASK);
}
static void fl_set_prev(free_list* fl,free_list* prev)
{
	return fl->prev=(free_list*)(((size_t)fl->prev&~FL_PREV_MASK)|((size_t)prev));
}

static free_list* fl_get_next(free_list* fl)
{
	return (free_list*)((size_t)fl->next&FL_PREV_MASK);
}
static void fl_set_next(free_list* fl,free_list* next)
{
	return fl->next=(free_list*)(((size_t)fl->next&~FL_PREV_MASK)|((size_t)next));
}

static void spinlock_lock(atomic_flag* flag)
{
	while(atomic_flag_test_and_set(flag));
}

static void spinlock_unlock(atomic_flag* flag)
{
	atomic_flag_clear(flag);
}

typedef struct arena {
	atomic_flag lock;
	free_list* buckets[CLASS_SIZE_COUNT];
} arena;

static size_t _Atomic current_arena=0;

static arena arenas[ARENA_COUNT];

static size_t log_2(size_t x)
{
	return 63-__builtin_clzll(x);
}

static size_t class_index(size_t _bytes)
{
	size_t const bytes=_bytes+8;
	if(bytes<16)
	{
		return 0;
	}
	size_t const l2=log_2(bytes-1);
	size_t const base=(l2-4)*2;
	size_t const mask=(((size_t)1)<<(l2-1));
	size_t const offset=bytes&mask;
	return base+1+!!offset;
}

static size_t div_up(size_t xx,size_t yy)
{
	return (xx+yy-1)/yy;
}

static size_t floor2(size_t x)
{
	return ((size_t)1)<<log_2(x);
}

static size_t get_arena_number()
{
	static __thread size_t arena_number=-1;
	if(arena_number==-1)
	{
		arena_number=atomic_fetch_add_explicit(&current_arena,1,memory_order_relaxed)%ARENA_COUNT;
	}
	return arena_number;
}

static void insert_block_at_front(memblock* bl,arena* ar,size_t class_index)
{
	free_list* head=ar->buckets[class_index];
	free_list* new_head=(free_list*)bl;
	ar->buckets[class_index]=new_head;
	new_head->used=0;
	fl_set_index(new_head,class_index);
	fl_set_next(new_head,head);
	fl_set_prev(new_head,0);
	if(head)
	{
		fl_set_prev(head,new_head);
	}
}

static void remove_block(free_list* bl,arena* ar,size_t class_index)
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

static memblock* split_block_insert(size_t arena_number,size_t block_class_index,memblock* block,size_t needed_index)
{
	arena* arena=arenas+arena_number+arena_number;
	if((needed_index-block_class_index)&1)
	{
		while(1)
		{
			if(needed_index+1>=block_class_index)
			{
				break;
			}
			else
			{
				size_t const split_index=block_class_index-2;
				memblock* right_part=(memblock*)((char*)block+class_sizes[split_index]);
				right_part->left=0;
				insert_block_at_front(right_part,arena,split_index);
				block_class_index=split_index;
			}
		}
	}
	else
	{
		while(1)
		{
			if(needed_index==block_class_index)
			{
				break;
			}
			else
			{
				size_t const split_index=block_class_index-2;
				memblock* right_part=(memblock*)((char*)block+class_sizes[split_index]);
				right_part->left=0;
				insert_block_at_front(right_part,arena,split_index);
				block_class_index=split_index;
			}
		}
	}
	block->size_index=needed_index;
	block->used=1;
	block->left=1;
	block->arena_source=arena_number;
	return block;
}

void* xmalloc(size_t const _bytes)
{
	if(_bytes==0)
	{
		return 0;
	}
	size_t const index=class_index(_bytes);
	if(index>=CLASS_SIZE_COUNT)
	{
		memblock* ret=mmap(0,(size_t)1<<(index/2),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
		ret->used=1;
		ret->size_index=index;
		return ret->data;
	}
	size_t const arena_number=get_arena_number();
	spinlock_lock(&arenas[arena_number].lock);
	for(size_t i=index;i<CLASS_SIZE_COUNT;++i)
	{
		free_list* chunk=arenas[arena_number].buckets[i];
		if(chunk)
		{
			size_t const class_size=class_sizes[i];
			free_list* next=fl_get_next(chunk);
			if(next)
			{
				fl_set_prev(next,0);
			}
			arenas[arena_number].buckets[i]=fl_get_next(chunk);
			memblock* ret=split_block_insert(arena_number,i,chunk,index);
			return ret->data;
		}
	}
	memblock* block=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
	memblock* ret=split_block_insert(arena_number,class_index(4096),block,index);
	return ret->data;
}

void xfree(void* ptr)
{
	if(ptr)
	{
		memblock* start=(memblock*)((char*)ptr-offsetof(memblock,data));
		size_t index=start->size_index;
		if(index>=CLASS_SIZE_COUNT)
		{
			munmap(start,get_class_size(index));
		}
		else
		{
			size_t const arena_number=start->arena_source;
			spinlock_lock(&arenas[arena_number].lock);
			free_list* to_insert=(free_list*)start;
			while(index<CLASS_SIZE_COUNT)
			{
				if(to_in)
				{
					free_list* buddy=(free_list*)((char*)start+get_class_size(index));
					if(!buddy->used&&fl_get_index(buddy)==start->buddy_size_index)
					{
						
					}
					else
					{
						insert_block_at_front(start,arenas+arena_number,index);
					}
				}
				else
				{
					free_list* buddy=(free_list*)((char*)start-get_class_size(start->buddy_size_index));
					if(!buddy->used&&fl_get_index(buddy)==start->buddy_size_index)
					{

					}
					else
					{
						insert_block_at_front(start,arenas+arena_number,index);
					}
				}
			}
			insert_block_at_front((memblock*)to_insert,arenas+arena_number,index);
			spinlock_unlock(&arenas[arena_number].lock);
		}
	}
}

void* xrealloc(void* prev,size_t bytes)
{
	if(prev)
	{
		memblock* start=(memblock*)((char*)prev-offsetof(memblock,data));
		size_t const size=class_sizes[start->size_index]-8;
		if(size<bytes)
		{
			return prev;
		}
		else
		{
			void* ret=xmalloc(bytes);
			memcpy(ret,prev,size);
			xfree(prev);
			return ret;
		}
	}
	return 0;
}

