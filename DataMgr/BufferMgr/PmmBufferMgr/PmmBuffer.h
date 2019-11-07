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

#ifndef PMMBUFFER_H
#define PMMBUFFER_H

#include "../Buffer.h"

namespace CudaMgr_Namespace {
class CudaMgr;
}

namespace Buffer_Namespace {
class PmmBuffer : public Buffer {
 public:
  PmmBuffer(BufferMgr* bm,
            BufferList::iterator segIt,
            const int deviceId,
            CudaMgr_Namespace::CudaMgr* cudaMgr,
            const size_t pageSize = 512,
            const size_t numBytes = 0);

  inline Data_Namespace::MemoryLevel getType() const override { return PMM_LEVEL; }

 private:
  void readData(int8_t* const dst,
                const size_t numBytes,
                const size_t offset = 0,
                const MemoryLevel dstMemoryLevel = PMM_LEVEL,
                const int dstDeviceId = -1) override;
  void writeData(int8_t* const src,
                 const size_t numBytes,
                 const size_t offset = 0,
                 const MemoryLevel srcMemoryLevel = PMM_LEVEL,
                 const int srcDeviceId = -1) override;
  CudaMgr_Namespace::CudaMgr* cudaMgr_;
};
}  // namespace Buffer_Namespace

#endif  // PMMBUFFER_H