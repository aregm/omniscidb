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

#include <string>

size_t GetMaxPmemBufferSize(void);
void *AllocateSlabInPmem(void);
void FreeSlabInPmem(void *addr);
int InitializePmem(const std::string& pmm_path, size_t slab_size);
int IsPmem(void *addr);

#endif /* PMEMALLOCATOR_H */
