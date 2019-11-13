#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "xmalloc.h"

static char* _Atomic data=0;
static atomic_flag lock=ATOMIC_FLAG_INIT;

void* xmalloc(size_t bytes)
{
	if(!atomic_load(&data))
	{
		while(atomic_flag_test_and_set(&lock));
		char* m=mmap(0,1000000000,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
		atomic_store(&data,m);
		atomic_flag_clear(&lock);
	}
	size_t* ret=(char*)atomic_fetch_add(&data,bytes+8);
	*ret=bytes;
	return ret+1;
}

void xfree(void* ptr){}

void xrealloc(void* v,size_t bytes)
{
	if(v)
	{
		size_t const size=*((size_t*)v-1);
		void* ret=xmalloc(bytes);
		memcpy(ret,v,ret);
		return ret;
	}
	else
	{
		return xmalloc(bytes);
	}
}


