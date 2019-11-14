#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include "xmalloc.h"

// from https://www.geeksforgeeks.org/branch-prediction-macros-in-gcc/
#define likely(x)      __builtin_expect(!!(x), 1) 
#define unlikely(x)    __builtin_expect(!!(x), 0) 

// This has the Nat Tuck seal of "not actually a stupid idea" 

static size_t div_up(size_t xx,size_t yy)
{
	return (xx+yy-1)/yy;
}

typedef struct free_list free_list;

typedef struct free_list_node {
	size_t size;
	struct free_list_node* next;
	struct free_list_node* prev;
} free_list_node;

typedef struct free_list {
	size_t cache_size;
	free_list_node* cache;
	free_list_node** cache_end;
	atomic_flag queue_lock;
	free_list_node* queue; // singly linked
	free_list_node* free; // doubly linked
	free_list* best_node;
} free_list;

void spinlock_lock(atomic_flag* lock)
{
	while(atomic_flag_test_and_set_explicit(lock,memory_order_acquire));
}

void spinlock_unlock(atomic_flag* lock)
{
	atomic_flag_clear_explicit(lock,memory_order_release);
}

typedef struct free_list_list {
	free_list* list;
	struct free_list_list* _Atomic next;
} free_list_list;

typedef struct memblock {
	size_t size;
	free_list* reserve;
	char data[];
} memblock;

free_list_list* _Atomic free_lists;

void push_free_list(free_list_list* node)
{
	while(1)
	{
		free_list_list* head=atomic_load(&free_lists);
		node->next=head;
		if(atomic_compare_exchange_strong(&free_lists,&head,node))
		{
			break;
		}
	}
}

static pthread_mutex_t gc_mtx=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc_cv=PTHREAD_COND_INITIALIZER;
static atomic_flag gc_init=ATOMIC_FLAG_INIT;
static pthread_t garbage_collector;

static free_list_node* next_block(free_list_node const* bl)
{
	return (free_list_node*)(((char*)bl)+bl->size);
}

static int coelescable(free_list_node const* a,free_list_node const* b)
{
	return (next_block(a)==b);
}

static void* cleanup(void* _)
{
	while(1)
	{
		pthread_mutex_lock(&gc_mtx);
		pthread_cond_wait(&gc_cv,&gc_mtx);
		for(free_list_list* fll=atomic_load(&free_lists);fll;fll=fll->next)
		{
			break;
			free_list* list=fll->list;
			free_list_node* to_insert;
			spinlock_lock(&list->queue_lock);
			to_insert=list->queue;
			list->queue=0;
			spinlock_unlock(&list->queue_lock);
			while(to_insert)
			{
				free_list* next=to_insert->next;
				if(list->free)
				{
					if(to_insert<list->free)
					{
						if(coelescable(to_insert,list->free))
						{
							to_insert->size+=list->free->size;
							to_insert->next=list->free->next;
							list->free=to_insert;
						}
						else
						{
							to_insert->next=list->free;
						}
					}
					else
					{
						free_list_node* prev=list->free;
						free_list_node* head=list->free->next;
						for(;head;prev=head,head=head->next)
						{
							if(to_insert<head)
							{
								if(coelescable(to_insert,head))
								{
									size_t const combined_size=to_insert->size+head->size;
									//printf("combined size %ld\n",combined_size);
									if(coelescable(prev,to_insert))
									{
										prev->size+=combined_size;
										prev->next=head->next;
									}
									else
									{
										to_insert->next=head->next;
										to_insert->size=combined_size;
										prev->next=to_insert;
									}
								}
								else if(coelescable(prev,to_insert))
								{
									prev->size+=to_insert->size;
								}
								else
								{
									prev->next=to_insert;
									to_insert->next=head;
								}
								goto end_loop;
							}
						}
						if(coelescable(prev,to_insert))
						{
							prev->size+=to_insert->size;
						}
						else
						{
							prev->next=to_insert;
							to_insert->next=head;
						}
					end_loop:;
					}
				}
				else
				{
					list->free=to_insert;
					list->free->next=0;
				}
				to_insert=next;
			}
		}
	}
	return 0;
}

static free_list* get_reserve()
{
	static __thread free_list fl={0,0,0,0,0,0};
	static __thread free_list_list fll={0,0};
	if(unlikely(fll.list==0))
	{
		fll.list=&fl;
		fl.cache_end=&fl.cache;
		push_free_list(&fll);
	}
	return &fl;
}

void* xmalloc(size_t _bytes)
{
	if(_bytes==0)
	{
		return 0;
	}
	static __thread int gc_initt=0;
	if(!gc_initt)
	{
		if(!atomic_flag_test_and_set(&gc_init))
		{
			pthread_create(&garbage_collector,0,cleanup,0);
		}
		gc_initt=1;
	}
	static __thread char* data=0;
	static __thread char* data_end=0;
	size_t const needed=div_up(_bytes+16,16)*16;
	free_list* reserve=get_reserve();
	if(reserve->cache)
	{
		free_list_node* el=reserve->cache;
		size_t const el_size=el->size;
		if(needed<=el_size)
		{
			free_list_node* next=el->next;
			memblock* ret=(memblock*)el;
			if(needed==el_size)
			{
				reserve->cache=next;
				if(next==0)
				{
					reserve->cache_end=&reserve->cache;
				}
			}
			else
			{
				size_t const remaining=el_size-needed;
				free_list_node* new_node=(free_list_node*)((char*)el+needed);
				new_node->next=0;
				new_node->size=remaining;
				(*reserve->cache_end)=new_node;
				reserve->cache_end=&new_node->next;
			}
			ret->size=needed;
			reserve->cache_size-=needed;
			ret->reserve=reserve;
			return ret->data;
		}
	}
	if(data+needed>data_end)
	{
		if(data)
		{
			char* last=(char*)div_up((size_t)data,4096);
			munmap(last,data_end-last);
		}
		size_t const block_size=0x100000ULL;
		size_t const to_alloc=block_size>needed?block_size:needed;
		data=mmap(0,to_alloc,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
		data_end=data+to_alloc;
	}
	memblock* ret=(memblock*)data;
	ret->size=needed;
	ret->reserve=reserve;
	data+=needed;
	return ret->data;
}

void xfree(void* ptr)
{
	if(ptr)
	{
		free_list_node* start=(char*)ptr-16;
		memblock* block=(memblock*)start;
		// put memory on thread local cache
		free_list* reserve=get_reserve();
		size_t const size=start->size;
		if(reserve->cache==0)
		{
			reserve->cache_end=&start->next;
		}
		start->next=reserve->cache;
		reserve->cache=start;
		size_t const CACHE_LIMIT=512;
		if(reserve->cache_size>=CACHE_LIMIT)
		{
			spinlock_lock(&reserve->queue_lock);
			(*reserve->cache_end)=reserve->queue;
			reserve->queue=reserve->cache;
			spinlock_unlock(&reserve->queue_lock);
			pthread_cond_signal(&gc_cv);
			reserve->cache=0;
			reserve->cache_end=&reserve->cache;
			reserve->cache_size=0;
		}
	}
}

void* xrealloc(void* v,size_t bytes)
{
	if(v)
	{
		size_t const size=*((size_t*)v-2);
		size_t const needed=div_up(bytes+16,16)*16;
		if(needed>size)
		{
			void* ret=xmalloc(bytes);
			memcpy(ret,v,size-16);
			xfree(v);
			return ret;
		}
		return v;
	}
	else
	{
		return xmalloc(bytes);
	}
}


