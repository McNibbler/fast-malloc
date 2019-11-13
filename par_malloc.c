#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "xmalloc.h"

void* xmalloc(size_t bytes)
{
	static __thread char* data=0;
	if(data==0)
	{
		data =mmap(0,10000000,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
	}
	size_t* ret=data;
	*ret=bytes;
	data+=bytes+8;
	return ret+1;
}

void xfree(void* ptr){}

void* xrealloc(void* v,size_t bytes)
{
	if(v)
	{
		size_t const size=*((size_t*)v-1);
		void* ret=xmalloc(bytes);
		memcpy(ret,v,size);
		return ret;
	}
	else
	{
		return xmalloc(bytes);
	}
}


