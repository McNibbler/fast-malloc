
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <pthread.h>
#include "xmalloc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

typedef struct memblock {
	size_t size;
	union {
		struct memblock* next;
		char data[1];
	};
} memblock,free_list;

size_t const PAGE_SIZE=4096;
size_t const MIN_ALLOC_SIZE=2*sizeof(size_t);
static free_list list_arenas[8];
static size_t _Atomic current_arena_number=0;
static atomic_flag list_mutex=ATOMIC_FLAG_INIT;


static void spinlock_lock(atomic_flag* flag)
{
	while(atomic_flag_test_and_set(flag));
}

static void spinlock_unlock(atomic_flag* flag)
{
	atomic_flag_clear(flag);
}

static size_t div_up(size_t xx,size_t yy)
{
	return (xx+yy-1)/yy;
}

static memblock* map_block(size_t size)
{
	memblock* ret=mmap(0,size,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
	ret->size=size;
	return ret;
}

static memblock* next_block(memblock const* bl)
{
	return (memblock*)(((char*)bl)+bl->size);
}

static int coelescable(memblock const* a,memblock const* b)
{
	return (next_block(a)==b);
	//&&((size_t)a/PAGE_SIZE==(size_t)b/PAGE_SIZE);
}

static size_t get_arena_number()
{
	static __thread size_t arena_number=-1;
	if(arena_number==-1)
	{
		arena_number=atomic_fetch_add(&current_arena_number,1);
	}
	return arena_number;
}

static free_list* get_arena()
{
	return list_arenas+get_arena_number();
}

static void insert_block_nonempty(memblock* block)
{
	/*puts(__FUNCTION__);
	printf("%x %x %ld\n",block,(void*)block->size,block->size);
	for(free_list* head=list.next;head;head=head->next)
	{
		printf("%x %x %ld %x\n",head,(void*)head->size,head->size,head->next);
	}*/

	free_list* prev=get_arena();
	free_list* head=prev->next;
	for(;head;prev=head,head=head->next)
	{
		if(block<head)
		{
			if(coelescable(block,head))
			{
				size_t const combined_size=block->size+head->size;
				//printf("combined size %ld\n",combined_size);
				if(coelescable(prev,block))
				{
					prev->size+=combined_size;
					prev->next=head->next;
				}
				else
				{
					block->next=head->next;
					block->size=combined_size;
					prev->next=block;
				}
			}
			else if(coelescable(prev,block))
			{
				prev->size+=block->size;
			}
			else
			{
				prev->next=block;
				block->next=head;
			}
			return;
		}
	}
	if(coelescable(prev,block))
	{
		prev->size+=block->size;
	}
	else
	{
		prev->next=block;
		block->next=head;
	}
}

static size_t fix_size(size_t size)
{
	if(size<MIN_ALLOC_SIZE-sizeof(size_t))
	{
		return MIN_ALLOC_SIZE;
	}
	return div_up(size+sizeof(size_t),sizeof(size_t))*sizeof(size_t);
}

void* xmalloc(size_t _size)
{
	/*puts(__FUNCTION__);
	printf("Requested: %ld\n",_size);
	for(free_list* head=list.next;head;head=head->next)
	{
		printf("%x %x %ld\n",head,(void*)head->size,head->size);
	}*/

	size_t const size=fix_size(_size);

	if(size>=PAGE_SIZE)
	{
		//printf("big %ld\n",size);
		memblock* block=map_block(size);
		return block->data;
	}
	else
	{
		spinlock_lock(&list_mutex);
		free_list* prev=get_arena();
		for(free_list* head=prev->next;head;prev=head,head=head->next)
		{
			size_t const block_size=head->size;
			if(block_size>=size)
			{
				void* ret=head->data;
				size_t const remaining_size=block_size-size;
				if(remaining_size<MIN_ALLOC_SIZE)
				{
					prev->next=head->next;
				}
				else
				{
					free_list* next=head->next;
					head->size=size;
					head=next_block(head);
					head->size=remaining_size;
					head->next=next;
					prev->next=head;
				}
				spinlock_unlock(&list_mutex);
				return ret;
			}
		}
		memblock* block=map_block(size);
		size_t const remaining_size=PAGE_SIZE-size;
		if(remaining_size>=MIN_ALLOC_SIZE)
		{
			memblock* rest=next_block(block);
			rest->size=remaining_size;
			insert_block_nonempty(rest);
		}
		else
		{
			block->size=PAGE_SIZE;
		}
		spinlock_unlock(&list_mutex);
		return block->data;
	}
}

void
xfree(void* item)
{

	memblock* block=(memblock*)((char*)item-sizeof(size_t));
	size_t const size=block->size;
	//printf("size free %ld\n",size);
	if(size>PAGE_SIZE)
	{
		munmap(block,size);
	}
	else
	{
		spinlock_lock(&list_mutex);
		insert_block_nonempty(block);
		spinlock_unlock(&list_mutex);
	}
}

void*
xrealloc(void* item,size_t _size)
{
	if(item==0)
	{
		return xmalloc(_size);
	}
	size_t const size=fix_size(_size);
	memblock* block=(memblock*)((char*)item-sizeof(size_t));
	size_t const block_size=block->size;
	if(block_size>=size)
	{
		return item;
	}
	else
	{
		void* data=xmalloc(_size);
		memcpy(data,item,block_size-sizeof(size_t));
		xfree(item);
		return data;
	}
}

