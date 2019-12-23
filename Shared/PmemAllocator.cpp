/*
 */

#include <stdio.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#include "Shared/PmemAllocator.h"

#define PMEM_DIR_1 "/mnt/ad1/zma2"
#define PMEM_DIR_2 "/mnt/ad2/zma2"

size_t slabsize = 4L * 1024 * 1024 * 1024; //4GB
struct PmemPoolDescriptor {
	void *base;
	void *ceiling;
	unsigned long *volatile allocated_flags;
	size_t size;
	size_t num_slabs;
	size_t volatile num_free_slabs;
};

struct PmemPoolDescriptor *pmem_pools;

//size_t * volatile bitmap;
//size_t numbitmaps;
//volatile size_t numfreeslabs;

unsigned int curpool = 0;
unsigned int numpools = 0;
size_t total_size = 0;

size_t
GetMaxPmemBufferSize(void)
{
	return total_size;
}

void *
AllocateSlabInPmem(void)
{
	size_t i;
	unsigned int index;

	index = __sync_val_compare_and_swap(&curpool, numpools, 0);
	if (index == numpools)
		index = 0;

	for (unsigned int j = 0; j < numpools; j++) {
		if (pmem_pools[index].num_free_slabs) {
			for (i = 0; i < pmem_pools[index].num_slabs; i++) {
				if (pmem_pools[index].allocated_flags[i] == 0) {
					if (__sync_bool_compare_and_swap(&(pmem_pools[index].allocated_flags[i]), 0, 1)) {
						//size_t left;
						//left = __sync_fetch_and_sub(&(pmem_pools[index].num_free_slabs), 1);
					
						//printf("%ld free slabs\n", left - 1);

						// well, not strictly interleaving
						__sync_fetch_and_add(&curpool, 1);

						return (void *)((char *)(pmem_pools[index].base) + slabsize * i);
					}
				}
			}
		}
	
		index++;
		if (index == numpools)
			index = 0;
	}
#if 0
	size_t oldv;
	size_t newv;
	while (numfreeslabs) {
	for (i = 0; i < numbitmaps; i++) {
		if (bitmap[i] != ~(size_t)(0)) {
			size_t j;
			for (j = 0; j < sizeof(size_t) * 8; j++) {
				if ((i * sizeof(size_t) * 8 + j) == numslabs)
					return NULL;
				oldv = bitmap[i];
				if ((oldv & (1 << j)) == 0) {
					newv = oldv | (1 << j);
					if (__sync_bool_compare_and_swap(&(bitmap[i]), oldv, newv)) {
						size_t left;

						left = __sync_fetch_and_sub(&numfreeslabs, 1);
						printf("%ld free slabs\n", left-1);
						return (void *)((char *)base + slabsize * (i * sizeof(size_t) * 8 + j));
					}
				}
			}
		}
	}		
	}
#endif /* 0 */


	printf("OUT OF PMEM\n");

	return NULL;
}

void
FreeSlabInPmem(void *addr)
{
	size_t i;

	//printf("free slab %p\n", addr);

	for (unsigned int j = 0; j < numpools; j++) {
		if (((char *)addr >= (char *)(pmem_pools[j].base)) && ((char *)addr < (char *)(pmem_pools[j].ceiling))) {
			i = (((char *)addr - (char *)(pmem_pools[j].base)) / slabsize);
			if (__sync_bool_compare_and_swap(&(pmem_pools[j].allocated_flags[i]), 1, 0)) {
				__sync_fetch_and_add(&(pmem_pools[j].num_free_slabs), 1);
			}
			else {
				printf("allcoated_flags corrupted\n");
			}
		}
	}
#if 0
	size_t j;

	i = (((char *)addr - (char *)base) / slabsize) / (sizeof(size_t) * 8);
	j = (((char *)addr - (char *)base) /slabsize) % (sizeof(size_t) * 8);

	while (1) {
		size_t oldv;
		size_t newv;

		oldv = bitmap[i];
		newv = oldv & (~(1 << j));
		if (__sync_bool_compare_and_swap(&(bitmap[i]), oldv, newv)) {
			__sync_fetch_and_add(&numfreeslabs, 1);
			return;
		}
	}
#endif /* 0 */
}

// TODO: read this information from a config file or command line options
//static char *dirs[2] = {PMEM_DIR_1, PMEM_DIR_2};
//static char *filename[2] = {"/mnt/ad1/zma2/omnisci.XXXXXX", "/mnt/ad2/zma2/omnisci.XXXXXX"};

int
InitializePmem(size_t slab_size)
{
	numpools = 2;
	slabsize = slab_size;

	pmem_pools = (struct PmemPoolDescriptor *)malloc(sizeof(struct PmemPoolDescriptor) * numpools);

	for (unsigned int i = 0; i < numpools; i++) {
		struct statfs buf;

		if (i == 0) {
		if (statfs(PMEM_DIR_1, &buf)) {
			printf("failed to initialize pmem\n");
			return -1;
		}
		printf("InitializePmem %s size=%ld\n", PMEM_DIR_1,  pmem_pools[i].size);
		}
		else {
		if (statfs(PMEM_DIR_2, &buf)) {
			printf("failed to initialize pmem\n");
			return -1;
		}
		printf("InitializePmem %s size=%ld\n", PMEM_DIR_2,  pmem_pools[i].size);
		}

		pmem_pools[i].size = buf.f_bavail * buf.f_bsize;
	


		int fd;
	
		char filename[128];

		if (i == 0) {
		sprintf(filename, "%s", "/mnt/ad1/zma2/omnisci.XXXXXX");
		}
		else
		{
		sprintf(filename, "%s", "/mnt/ad2/zma2/omnisci.XXXXXX");
		}


		if ((fd = mkstemp(filename)) < 0) {
			printf("mkstemp fialed\n");
			exit(-1);
		}
		unlink(filename);

		int ret;
	
		pmem_pools[i].num_slabs = pmem_pools[i].size/slab_size;
		pmem_pools[i].size = pmem_pools[i].num_slabs * slabsize; 
		total_size +=  pmem_pools[i].size;

		if ((ret = posix_fallocate(fd, 0, pmem_pools[i].size)) != 0) {
			printf("posix_fallcoate failed err=%d\n", ret);
			exit(-1);
		}

		if ((pmem_pools[i].base = mmap(NULL, pmem_pools[i].size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
			printf("mmap filed\n");
			return -1;
		}
 
		pmem_pools[i].ceiling = (void *)((char *)(pmem_pools[i].base) + pmem_pools[i].size);

	
		//if (munmap(base, size)) {
		//	printf("munmap failed\n");
		//	exit(-1);
		//}

	
		pmem_pools[i].num_free_slabs = pmem_pools[i].num_slabs;

	
		//bitmap = (size_t *)malloc((numslabs  + sizeof(size_t) * 8 - 1 ) / (sizeof(size_t) * 8 ) * sizeof(size_t));
		//numbitmaps = (numslabs  + sizeof(size_t) * 8 - 1 )/ (sizeof(size_t) * 8);

	
		pmem_pools[i].allocated_flags = (unsigned long *)malloc(pmem_pools[i].num_slabs * sizeof(unsigned long));

	
		//for (i = 0; i < numbitmaps; i++) {
		//	bitmap[i] = 0;
		//}

	
		for (unsigned int j = 0; j < pmem_pools[i].num_slabs; j++) {
			pmem_pools[i].allocated_flags[j] = 0;
		}

	
		//printf("pmem initialzied numbitmaps = %ld numslabs=%ld\n", numbitmaps, numslabs);
	
		printf("pmem initialzied numslabs=%ld\n", pmem_pools[i].num_slabs);
	}

	return 0;
}

int
IsPmem(void *addr)
{
	for (unsigned int i = 0; i < numpools; i++) {
		if (((char *)addr >= (char *)(pmem_pools[i].base)) && ((char *)addr < (char *)(pmem_pools[i].ceiling))) 
			return 1;
	}
	return 0;
}

#if 0
int
InitializePmem(void)
{
	int ret;

	ret = memkind_create_pmem(PMEM_DIR, 700L * 1024 * 1024 * 1024, &pmem_kind);

	if (ret) {
		printf("failed to initialzie pmem\n");
	}
	
       return ret;	
}
#endif /* 0 */

