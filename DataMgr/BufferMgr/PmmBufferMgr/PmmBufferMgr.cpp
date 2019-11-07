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

#include "PmmBufferMgr.h"
#include <glog/logging.h>
#include "../../../CudaMgr/CudaMgr.h"
#include "PmmBuffer.h"
#include "Shared/PmemAllocator.h"

//#include <memkind.h>

#include <stdio.h>
namespace Buffer_Namespace {

PmmBufferMgr::PmmBufferMgr(const int deviceId,
                           const size_t maxBufferSize,
                           CudaMgr_Namespace::CudaMgr* cudaMgr,
			   AbstractBufferMgr *cpuMgr,
                           const size_t bufferAllocIncrement,
                           const size_t pageSize,
                           AbstractBufferMgr* parentMgr)
    : BufferMgr(deviceId, maxBufferSize, bufferAllocIncrement, pageSize, parentMgr)
    , cudaMgr_(cudaMgr)
    , cpuMgr_(cpuMgr)	{}

PmmBufferMgr::~PmmBufferMgr() {
  freeAllMem();
}

#include <sys/sysinfo.h>

#if 0
int IsToAllocateInDram(size_t size) {
	struct sysinfo info;

	if (sysinfo(&info))
		return 0;

	if (((info.freeram + info.bufferram) * 4  >  info.totalram) && ((info.freeram + info.bufferram) * info.mem_unit > size))
		return 1;
	else
		return 0;
}
#endif /* 0 */

void PmmBufferMgr::addSlab(const size_t slabSize) {
  slabs_.resize(slabs_.size() + 1);
  printf("add slab %ld\n", slabSize);
  try {
  
			  void *p;

			  printf("allocat slab on pmem\n");
			  //p = memkind_malloc(pmem_kind, slabSize);
			  p = AllocateSlabInPmem();
		  
			  printf("slab=%p\n", p);
			  if (p == NULL) {
				  printf("Out of pmem\n");
				  throw std::bad_alloc();
		  
			  }
			  else {
				  slabs_.back() = (int8_t *)p;
			  }
  } catch (std::bad_alloc&) {
    slabs_.resize(slabs_.size() - 1);
    throw FailedToCreateSlab(slabSize);
  }
  slabSegments_.resize(slabSegments_.size() + 1);
  slabSegments_[slabSegments_.size() - 1].push_back(BufferSeg(0, slabSize / pageSize_));
}

void PmmBufferMgr::freeAllMem() {
  for (auto bufIt = slabs_.begin(); bufIt != slabs_.end(); ++bufIt) {
	  if (IsPmem(*bufIt)) {
		  // invoke destructor
		  //
		  //memkind_free(pmem_kind, (void *)*bufIt);
		  FreeSlabInPmem((void *)*bufIt);
	  }
	  else {
		  delete[] * bufIt;
	  }
  }
}

void PmmBufferMgr::allocateBuffer(BufferList::iterator segIt,
                                  const size_t pageSize,
                                  const size_t initialSize) {
  new PmmBuffer(this,
                segIt,
                deviceId_,
                cudaMgr_,
                pageSize,
                initialSize);  // this line is admittedly a bit weird but
                               // the segment iterator passed into buffer
                               // takes the address of the new Buffer in its
                               // buffer member
}

}  // namespace Buffer_Namespace
