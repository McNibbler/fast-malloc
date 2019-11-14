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

// This has the Nat Tuck seal of "not actually a stupid idea."
// Can be interpreted as a simplified version of Google's TCMalloc
// Each thread allocates a block of memory to take from.
// On freeing, memory segments are consed to the front of a
// thread-local free list, if the cache grows too big
// it is sent to a garbage collector thread that will
// coealesce memory and post it to a global heap.
// On allocation, memory is taken from the free list,
// from the block of data available, from the global heap,
// or newly allocated, in that order.

static size_t div_up(size_t xx,size_t yy)
{
	return (xx+yy-1)/yy;
}

typedef struct free_list_node {
	size_t size;
	struct free_list_node* next;
	//struct free_list_node* prev;
} free_list_node;

typedef struct local_reserve {
	size_t cache_size;
	free_list_node* cache;
	free_list_node** cache_end;
	atomic_flag queue_lock;
	free_list_node* queue; // singly linked, how the cache is given to the garbage collector
} local_reserve;

static void spinlock_lock(atomic_flag* lock)
{
	while(atomic_flag_test_and_set_explicit(lock,memory_order_acquire));
}

static void spinlock_unlock(atomic_flag* lock)
{
	atomic_flag_clear_explicit(lock,memory_order_release);
}

typedef struct reserve_list {
	local_reserve* list;
	struct reserve_list* _Atomic next;
} reserve_list;

typedef struct memblock {
	size_t size;
	size_t _padding;
	char data[];
} memblock;

static reserve_list* _Atomic free_lists;

static void push_free_list(reserve_list* node)
{
	while(1)
	{
		reserve_list* head=atomic_load(&free_lists);
		node->next=head;
		if(atomic_compare_exchange_strong(&free_lists,&head,node))
		{
			break;
		}
	}
}

static free_list_node* global_heap=0;
static atomic_flag heap_lock=ATOMIC_FLAG_INIT;

static pthread_mutex_t gc_mtx=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc_cv=PTHREAD_COND_INITIALIZER;
static atomic_flag gc_init=ATOMIC_FLAG_INIT;
static atomic_size_t awakenings=ATOMIC_VAR_INIT(0);
static pthread_t garbage_collector;

static free_list_node* next_block(free_list_node const* bl)
{
	return (free_list_node*)(((char*)bl)+bl->size);
}

static int coelescable(free_list_node const* a,free_list_node const* b)
{
	return (next_block(a)==b);
}

enum constants {
	PAGE_SIZE=4096
};

static int page_aligned(void* addr)
{
	return (((size_t)addr)%PAGE_SIZE)==0;
}

static free_list_node* sort_free_list(free_list_node* head)
{
	if(head==0||head->next==0)
	{
		return head;
	}
	free_list_node* next=head->next;
	if(next->next==0)
	{
		if(head->size<next->size)
		{
			next->next=head;
			head->next=0;
			return next;
		}
		else
		{
			return head;
		}
	}
	free_list_node* before_second_half=next;
	free_list_node* far=next->next;
	while(far&&(far=far->next))
	{
		before_second_half=before_second_half->next;
	}
	free_list_node* second_half=before_second_half->next;
	before_second_half->next=0;
	second_half=sort_free_list(second_half);
	head=sort_free_list(head);
	free_list_node* ret=0;
	free_list_node** prev=&ret;
	while(1)
	{
		if(second_half==0)
		{
			*prev=head;
			break;
		}
		if(head==0)
		{
			*prev=second_half;
			break;
		}
		if(head->size>second_half->size)
		{
			*prev=head;
			prev=&head->next;
			head=head->next;
		}
		else
		{
			*prev=second_half;
			prev=&second_half->next;
			second_half=second_half->next;
		}
	}
	return ret;
}

static void* cleanup(void* _)
{
	free_list_node* deleted=0;
	while(1)
	{
		pthread_mutex_lock(&gc_mtx);
		while(atomic_load_explicit(&awakenings,memory_order_acquire)==0)
		{
			pthread_cond_wait(&gc_cv,&gc_mtx);
		}
		atomic_store_explicit(&awakenings,0,memory_order_release);
		for(reserve_list* fll=atomic_load(&free_lists);fll;fll=fll->next)
		{
			local_reserve* list=fll->list;
			free_list_node* to_insert;
			spinlock_lock(&list->queue_lock);
			to_insert=list->queue;
			list->queue=0;
			spinlock_unlock(&list->queue_lock);
			while(to_insert)
			{
				free_list_node* next=to_insert->next;
				if(deleted)
				{
					if(to_insert<deleted)
					{
						if(coelescable(to_insert,deleted))
						{
							to_insert->size+=deleted->size;
							to_insert->next=deleted->next;
							deleted=to_insert;
						}
						else
						{
							to_insert->next=deleted;
						}
					}
					else
					{
						free_list_node* prev=deleted;
						free_list_node* head=deleted->next;
						for(;head;prev=head,head=head->next)
						{
							if(to_insert<head)
							{
								if(coelescable(to_insert,head))
								{
									size_t const combined_size=to_insert->size+head->size;
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
					deleted=to_insert;
					deleted->next=0;
				}
				to_insert=next;
			}
			free_list_node* sorted=sort_free_list(deleted);
			spinlock_lock(&heap_lock);
			deleted=global_heap;
			global_heap=sorted;
			spinlock_unlock(&heap_lock);
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
	}
	return 0;
}

static local_reserve* get_reserve()
{
	static __thread local_reserve fl={0,0,0,ATOMIC_FLAG_INIT,0};
	static __thread reserve_list fll={0,ATOMIC_VAR_INIT((void*)0)};
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
	if(unlikely(_bytes==0))
	{
		return 0;
	}
	static __thread int gc_initt=0;
	if(unlikely(!gc_initt))
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
	local_reserve* reserve=get_reserve();
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
			return ret->data;
		}
	}
	if(unlikely(data+needed>data_end))
	{
		spinlock_lock(&heap_lock);
		free_list_node* head=global_heap;
		if(head&&head->size>=needed)
		{
			global_heap=head->next;
			spinlock_unlock(&heap_lock);
			size_t const remaining=head->size-needed;
			if(remaining==0)
			{
				memblock* ret=(memblock*)head;
				return ret->data;
			}
			else 
			{
				memblock* ret=(memblock*)head;
				ret->size=needed;
				free_list_node* left=(free_list_node*)((char*)head+needed);
				left->size=remaining;
				if(reserve->cache==0)
				{
					reserve->cache=left;
					left->next=0;
					reserve->cache_end=&left->next;
				}
				else
				{
					if(remaining<reserve->cache->size)
					{
						(*reserve->cache_end)=left;
						left->next=0;
						reserve->cache_end=&left->next;
					}
					else
					{
						free_list_node* next=reserve->cache;
						reserve->cache=left;
						left->next=next;
					}
				}
				return ret->data;
			}
		}
		spinlock_unlock(&heap_lock);
		if(data)
		{
			char* last=(char*)(div_up((size_t)data,PAGE_SIZE)*PAGE_SIZE);
			munmap(last,data_end-last);
		}
		size_t const block_size=64*PAGE_SIZE;
		size_t const to_alloc=block_size>needed?block_size:needed;
		data=mmap(0,to_alloc,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
		data_end=data+to_alloc;
	}
	memblock* ret=(memblock*)data;
	ret->size=needed;
	data+=needed;
	return ret->data;
}

void xfree(void* ptr)
{
	if(likely(ptr))
	{
		free_list_node* start=(free_list_node*)((char*)ptr-16);
		memblock* block=(memblock*)start;
		// put memory on thread local cache
		// memory is more likely to be larger than previous allocations or the same size
		// so putting it on the front is a good guess
		local_reserve* reserve=get_reserve();
		size_t const size=start->size;
		if(reserve->cache==0)
		{
			reserve->cache_end=&start->next;
		}
		start->next=reserve->cache;
		reserve->cache=start;
		size_t const CACHE_LIMIT=PAGE_SIZE;
		if(reserve->cache_size>=CACHE_LIMIT)
		{
			spinlock_lock(&reserve->queue_lock);
			(*reserve->cache_end)=reserve->queue;
			reserve->queue=reserve->cache;
			spinlock_unlock(&reserve->queue_lock);
			atomic_fetch_add_explicit(&awakenings,1,memory_order_release);
			pthread_cond_signal(&gc_cv);
			reserve->cache=0;
			reserve->cache_end=&reserve->cache;
			reserve->cache_size=0;
		}
	}
}

void* xrealloc(void* v,size_t bytes)
{
	if(likely(v))
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
