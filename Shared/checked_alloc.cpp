/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#define PMEM_DIR "/mnt/ad2/zma2"
//#define PMEM_DIR "/tmp"
//
size_t slabsize = 4L * 1024 * 1024 * 1024; //4GB
void *base;
void *ceiling;
size_t numslabs;

//size_t * volatile bitmap;
//size_t numbitmaps;
//volatile size_t numfreeslabs;

unsigned int * volatile allocated_flags;
volatile size_t numfreeslabs;

void *pmem_kind = NULL;

void *
AllocateSlabInPmem(void)
{
	size_t i;

	while (numfreeslabs) {
		for (i = 0; i < numslabs; i++) {
			if (allocated_flags[i] == 0) {
				if (__sync_bool_compare_and_swap(&(allocated_flags[i]), 0, 1)) {
					size_t left;
						
					left = __sync_fetch_and_sub(&numfreeslabs, 1);
				
					printf("%ld free slabs\n", left - 1);
				
					return (void *)((char *)base + slabsize * i);
				}
			}
		}
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


	return NULL;
}

void
FreeSlabInPmem(void *addr)
{
	size_t i;

	printf("free slab %p\n", addr);

	i = (((char *)addr - (char *)base) / slabsize);

	if (__sync_bool_compare_and_swap(&(allocated_flags[i]), 1, 0)) {
		__sync_fetch_and_add(&numfreeslabs, 1);
	}
	else {
		printf("allcoated_flags corrupted\n");
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

int
InitializePmem(size_t size)
{
	printf("InitializePmem size=%ld\n", size);

	static char filename[] = "/mnt/ad2/zma2/omnisci.XXXXXX";
	int fd;

	if ((fd = mkstemp(filename)) < 0) {
		printf("mkstemp fialed\n");
		exit(-1);
	}


	unlink(filename);

	int ret;

	if ((ret = posix_fallocate(fd, 0, size)) != 0) {
		printf("posix_fallcoate failed err=%d\n", ret);
		exit(-1);
	}

    
	if ((base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		printf("mmap filed\n");
		exit(-1);
	}
 
	ceiling = (void *)((char *)base + size);

	//if (munmap(base, size)) {
	//	printf("munmap failed\n");
	//	exit(-1);
	//}

	numslabs = size/slabsize;
	numfreeslabs = numslabs;

	//bitmap = (size_t *)malloc((numslabs  + sizeof(size_t) * 8 - 1 ) / (sizeof(size_t) * 8 ) * sizeof(size_t));
	//numbitmaps = (numslabs  + sizeof(size_t) * 8 - 1 )/ (sizeof(size_t) * 8);

	allocated_flags = (unsigned int *)malloc(numslabs * sizeof(unsigned int));

	size_t i;

	//for (i = 0; i < numbitmaps; i++) {
	//	bitmap[i] = 0;
	//}

	for (i = 0; i < numslabs; i++) {
		allocated_flags[i] = 0;
	}

	pmem_kind = base;
	//printf("pmem initialzied numbitmaps = %ld numslabs=%ld\n", numbitmaps, numslabs);
	printf("pmem initialzied numslabs=%ld\n", numslabs);

	return 0;
}

int
IsPmem(void *addr)
{
	if (pmem_kind == NULL)
		return 0;

	if (((char *)addr >= (char *)base) && ((char *)addr < (char *)ceiling)) 
		return 1;
	else
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

