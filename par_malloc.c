#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include "xmalloc.h"

static size_t div_up(size_t xx,size_t yy)
{
	return (xx+yy-1)/yy;
}

typedef struct free_list {
	size_t size;
	struct free_list* _Atomic next;
} free_list;

static atomic_flag queue_lock=ATOMIC_FLAG_INIT;
static size_t free_list_size=0;
static free_list* queue;
static free_list* to_kill;

static pthread_mutex_t gc_mtx=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc_cv=PTHREAD_COND_INITIALIZER;
static atomic_flag gc_init=ATOMIC_FLAG_INIT;
static pthread_t garbage_collector;

static free_list* next_block(free_list const* bl)
{
	return (free_list*)(((char*)bl)+bl->size);
}

static int coelescable(free_list const* a,free_list const* b)
{
	return (next_block(a)==b);
}

static void* cleanup(void* _)
{
	while(1)
	{
		pthread_mutex_lock(&gc_mtx);
		pthread_cond_wait(&gc_cv,&gc_mtx);
		free_list* temp;
		while(atomic_flag_test_and_set(&queue_lock));
		free_list_size=0;
		temp=queue;
		queue=0;
		atomic_flag_clear(&queue_lock);
		while(temp)
		{
			free_list* next=temp->next;
			if(to_kill)
			{
				if(temp<to_kill)
				{
					if(coelescable(temp,to_kill))
					{
						temp->size+=to_kill->size;
						temp->next=to_kill->next;
						to_kill=temp;
					}
					else
					{
						temp->next=to_kill;
					}
				}
				else
				{
					free_list* prev=to_kill;
					free_list* head=to_kill->next;
					for(;head;prev=head,head=head->next)
					{
						if(temp<head)
						{
							if(coelescable(temp,head))
							{
								size_t const combined_size=temp->size+head->size;
								//printf("combined size %ld\n",combined_size);
								if(coelescable(prev,temp))
								{
									prev->size+=combined_size;
									prev->next=head->next;
								}
								else
								{
									temp->next=head->next;
									temp->size=combined_size;
									prev->next=temp;
								}
							}
							else if(coelescable(prev,temp))
							{
								prev->size+=temp->size;
							}
							else
							{
								prev->next=temp;
								temp->next=head;
							}
							goto end_loop;
						}
					}
					if(coelescable(prev,temp))
					{
						prev->size+=temp->size;
					}
					else
					{
						prev->next=temp;
						temp->next=head;
					}
				end_loop:;
				}
			}
			else
			{
				to_kill=temp;
				to_kill->next=0;
			}
			temp=next;
		}
	}
	return 0;
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
		while(atomic_flag_test_and_set(&gc_init));
		pthread_create(&garbage_collector,0,cleanup,0);
		atomic_flag_clear(&gc_init);
		gc_initt=1;
	}
	static __thread char* data=0;
	static __thread char* data_end=0;
	size_t needed=div_up(_bytes+16,16)*16;
	if(data+needed>data_end)
	{
		if(data)
		{
			char* last=(char*)div_up((size_t)data,4096);
			munmap(last,data_end-last);
		}
		size_t const block_size=0x100000000ULL;
		data=mmap(0,block_size,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
		data_end=data+block_size;
	}
	size_t* ret=(size_t*)data;
	*ret=needed;
	data+=needed;
	return ret+2;
}

void xfree(void* ptr)
{
	if(ptr)
	{
		free_list* start=(char*)ptr-16;
		while(atomic_flag_test_and_set(&queue_lock));
		start->next=queue;
		queue=start;
		free_list_size+=start->size;
		if(free_list_size>=4096)
		{
			pthread_cond_signal(&gc_cv);
		}
		atomic_flag_clear(&queue_lock);
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


