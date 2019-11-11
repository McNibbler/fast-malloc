#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdatomic.h>
#include <threads.h>
#include <string.h>
#include "xmalloc.h"

uint32_t const class_sizes[]={
	0x0010,
	0x0020,
	0x0040,
	0x0080,
	0x0100,
	0x0200,
	0x0400,
	0x0800,
	0x1000
};

enum constants {
	CLASS_SIZE_COUNT=9,
	ARENA_COUNT=4
};

static size_t const BIG_LIMIT=0x10000;

typedef struct memblock {
	union {
		struct {
			size_t size;
			char data[1];
		};
		struct {
			struct memblock* prev;
			struct memblock* next;
		};
	};
} memblock,free_list;

void spinlock_lock(atomic_flag volatile* flag)
{
	while(atomic_flag_test_and_set(flag));
}

void spinlock_unlock(atomic_flag volatile* flag)
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

static size_t class_index(size_t bytes)
{
	return log_2(bytes-1)-4;
}

static size_t div_up(size_t xx,size_t yy)
{
	return (xx+yy-1)/yy;
}

static size_t floor2(size_t x)
{
	return ((size_t)1)<<log_2(x);
}

static size_t fix_size(size_t size)
{
	if(size<16)
	{
		return 16;
	}
	return ((size_t)(1))<<(log_2(size-1)+1);
}

void insert_block_no_coelesce(size_t arena_index,size_t class_index,memblock* block)
{
	free_list* head=arenas[arena_index].buckets[class_index];
	if(head)
	{
		if(block<head)
		{
			arenas[arena_index].buckets[class_index]=block;
			block->next=head;
			block->prev=0;
			head->prev=block;
		}
		else
		{
			while(head->next)
			{
				if(block<head->next)
				{
					block->next=head->next;
					head->next=block;
					block->prev=head;
					return;
				}
				head=head->next;
			}
			head->next=block;
			block->prev=head;
			block->next=0;
		}
	}
	else
	{
		arenas[arena_index].buckets[class_index]=block;
		block->prev=0;
		block->next=0;
	}
}

size_t get_arena_number()
{
	static thread_local size_t arena_number=-1;
	if(arena_number==-1)
	{
		arena_number=atomic_fetch_add_explicit(&current_arena,1,memory_order_relaxed)%ARENA_COUNT;
	}
	return arena_number;
}

void* xmalloc(size_t const _bytes)
{
	if(_bytes==0)
	{
		return 0;
	}
	size_t const bytes=fix_size(_bytes);
	if(bytes>=BIG_LIMIT)
	{
		size_t* ret=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
		*ret=bytes;
		return ret+1;
	}
	size_t const arena_number=get_arena_number();
	spinlock_lock(&arenas[arena_number].lock);
	size_t const index=class_index(bytes);
	for(size_t i=index;i<CLASS_SIZE_COUNT;++i)
	{
		memblock* chunk=arenas[arena_number].buckets[i];
		if(chunk)
		{
			size_t const class_size=class_sizes[i];
			size_t remaining=class_size-bytes;
			memblock* remaining_chunk=(memblock*)((char*)chunk+class_size);
			while(remaining>0)
			{
				size_t const subclass_size=floor2(remaining);
				size_t const current_chunk_offset=remaining-subclass_size;
				size_t const index=class_index(subclass_size);
				memblock* to_insert=(memblock*)((char*)remaining_chunk+current_chunk_offset);
				insert_block_no_coelesce(arena_number,index,to_insert);
				remaining-=current_chunk_offset;
			}
			spinlock_unlock(&arenas[arena_number].lock);
			chunk->size=class_size;
			return chunk->data;
		}
	}
}

int coelesceable(memblock* prev,memblock* next,size_t class_index)
{
	return (size_t)prev^(size_t)next==(size_t)1<<class_index+4;
}

static void insert_block(size_t arena_number,size_t class_index,memblock* block)
{
	if(class_index+1>=CLASS_SIZE_COUNT)
	{
		return insert_block_no_coelesce(arena_number,class_index,block);
	}
	free_list* head=arenas[arena_number].buckets[class_index];
	if(head)
	{
		if(block<head)
		{
			if(coelesceable(block,head,class_index))
			{
				arenas[arena_number].buckets[class_index]=head->next;
				if(head->next)
				{
					head->next->prev=0;
				}
				return insert_block(arena_number,class_index+1,block);
			}
		}
		else
		{
			while(head->next)
			{
				if(block<head->next)
				{
					if(coelesceable(block,head->next,class_index))
					{
						head->next=head->next->next;
						if(head->next->next)
						{
							head->next->next->prev=head;
						}
						return insert_block(arena_number,class_index+1,block);
					}
					else
					{
						block->next=head->next;
						block->next->prev=block;
						head->next=block;
						block->prev=head;
						return;
					}
				}
				head=head->next;
			}
			if(coelesceable(head,block,class_index))
			{
				if(head->prev)
				{
					head->prev->next=head->next;
				}
				else
				{
					arenas[arena_number].buckets[class_index]=head->next;
				}
				return insert_block(arena_number,class_index+1,head);
			}
			else
			{
				block->next=0;
				block->prev=head;
				head->next=block;
			}
		}
	}
	else
	{
		arenas[arena_number].buckets[class_index]=block;
		block->next=0;
		block->prev=0;
	}
}

void xfree(void* ptr)
{
	if(ptr)
	{
		memblock* start=(memblock*)((char*)ptr-offsetof(memblock,data));
		size_t const size=start->size;
		if(size>=BIG_LIMIT)
		{
			munmap(start,size);
		}
		else {
			size_t const arena_number=get_arena_number();
			spinlock_lock(&arenas[arena_number].lock);
			size_t const index=class_index(size);
			insert_block(arena_number,index,start);
			spinlock_unlock(&arenas[arena_number].lock);
		}
	}
}

void* xrealloc(void* prev,size_t bytes)
{
	if(prev)
	{
		memblock* start=(memblock*)((char*)prev-offsetof(memblock,data));
		size_t const size=start->size;
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

