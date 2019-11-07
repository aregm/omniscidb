/*
 */

#ifndef PMEMALLOCATOR_H
#define PMEMALLOCATOR_H

#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

size_t GetMaxPmemBufferSize(void);
void *AllocateSlabInPmem(void);
void FreeSlabInPmem(void *addr);
int InitializePmem(size_t buffer_size, size_t slab_size);
int IsPmem(void *addr);

#endif /* PMEMALLOCATOR_H */
