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

/**
 * @file    DataMgr.cpp
 * @author Todd Mostak <todd@mapd.com>
 */

#include "Catalog/SysCatalog.h"
#include "DataMgr.h"
#include "../CudaMgr/CudaMgr.h"
#include "BufferMgr/CpuBufferMgr/CpuBufferMgr.h"
#include "BufferMgr/PmmBufferMgr/PmmBufferMgr.h"
#include "BufferMgr/GpuCudaBufferMgr/GpuCudaBufferMgr.h"
#include "FileMgr/GlobalFileMgr.h"
#include "Shared/PmemAllocator.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

#include <boost/filesystem.hpp>

#include <algorithm>
#include <limits>

using namespace std;
using namespace Buffer_Namespace;
using namespace File_Namespace;
using namespace Catalog_Namespace;

size_t GetMaxPmemBufferSize(void);

namespace Data_Namespace {

DataMgr::DataMgr(const string& dataDir,
                 const MapDParameters& mapd_parameters,
		 const bool pmm,
		 const std::string& pmm_path,
                 const bool useGpus,
                 const int numGpus,
                 const int startGpu,
                 const size_t reservedGpuMem,
                 const size_t numReaderThreads)
    : dataDir_(dataDir) {
  if (useGpus) {
    try {
      cudaMgr_ = std::make_unique<CudaMgr_Namespace::CudaMgr>(numGpus, startGpu);
      reservedGpuMem_ = reservedGpuMem;
      hasGpus_ = true;
    } catch (std::runtime_error& error) {
      hasGpus_ = false;
    }
  } else {
    hasGpus_ = false;
  }

  hasPmm_ = false;
  profSF_ = mapd_parameters.prof_scale_factor;
  if (pmm) {
	  if (InitializePmem(pmm_path, (1L << 32)) == 0) {
	    hasPmm_ = true;
	    printf("Use AppDirect\n");
	  }
  }

  statisticsOn_ = false;

  populateMgrs(mapd_parameters, numReaderThreads);
  createTopLevelMetadata();
}

void
DataMgr::startCollectingStatistics(void)
{
	std::unique_lock<std::mutex> chunkFetchStatsLock(chunkFetchStatsMutex_);
	// reset current database or all databases?
	SysCatalog::instance().clearDataMgrStatistics(hasPmm_);
	statisticsOn_ = true;
	chunkFetchStatsLock.unlock();

	std::cout << "Data manager statistics on." << std::endl;
}

bool
DataMgr::isPmemUsed(void)
{
	return hasPmm_;
}

int
DataMgr::getProfileScaleFactor(void)
{
	return profSF_;
}

size_t
DataMgr::getPeakVmSize(void)
{
	FILE *fp;
	char statusFileName[256];

	sprintf(statusFileName, "/proc/%d/status", getpid());
	fp = fopen(statusFileName, "r");
	if (fp == NULL) {
		printf("Cannot get peak vm size\n");
		return 0;
	}


	char token[128];
	size_t peakVmSize;
	while (!feof(fp)) {
		fscanf(fp, "%s", token);
		if (strcmp(token, "VmPeak:") == 0) {
			fscanf(fp, "%lu", &peakVmSize);
			fclose(fp);
			return peakVmSize * 1024;
		}
	}
	return 0;
}

void
DataMgr::stopCollectingStatistics(std::map<unsigned long, long>& query_time)
{

	std::map<unsigned long, std::map<std::vector<int>, size_t>> queryColumnFetchStats;	// number of times column fetched in the query
	std::map<unsigned long, std::map<std::vector<int>, size_t>> queryColumnChunkStats;	// number of unique chunks fetched in the query
	std::map<unsigned long, std::map<std::vector<int>, size_t>> queryColumnFetchDataSizeStats;	// size of data fetched in the query

	std::map<std::vector<int>, size_t> columnFetchStats;	// aggregated number of times columns fetched
	std::map<std::vector<int>, size_t> columnChunkStats;	// aggregated number of unique columns fetched
	std::map<std::vector<int>, size_t> columnFetchDataSizeStats;	// aggregated size of data fetched

	std::unique_lock<std::mutex> chunkFetchStatsLock(chunkFetchStatsMutex_);

	if (statisticsOn_) {
		for (std::map<unsigned long, std::map<std::vector<int>, size_t>>::const_iterator itmom = chunkFetchStats_.begin(); itmom != chunkFetchStats_.end(); itmom++) {
			// aggrgate chunk fetch stats by columns
			std::map<std::vector<int>, size_t> queryColumns;
			std::map<std::vector<int>, size_t> queryChunks;
			std::map<std::vector<int>, size_t> queryData;

			unsigned long query_id = itmom->first;
			queryColumnFetchStats[query_id] = queryColumns;
			queryColumnChunkStats[query_id] = queryChunks;
			queryColumnFetchDataSizeStats[query_id] = queryData;


			std::map<std::vector<int>, size_t>::const_iterator itm;
			for (itm = itmom->second.begin(); itm != itmom->second.end(); ++itm) {
				std::vector<int> key;

				key = itm->first;
				key.pop_back();	// pop off chunk id	

				std::map<std::vector<int>, size_t>::iterator itm2;
		
				itm2 = columnFetchStats.find(key);
				if (itm2 != columnFetchStats.end()) {
					itm2->second += itm->second;
					columnChunkStats[key] += 1;
					columnFetchDataSizeStats[key] += chunkFetchDataSizeStats_[query_id][itm->first];
				}
				else {
					columnFetchStats[key] = itm->second;
					columnChunkStats[key] = 1;
					columnFetchDataSizeStats[key] = chunkFetchDataSizeStats_[query_id][itm->first];
				}

				std::map<unsigned long, std::map<std::vector<int>, size_t>>::iterator itmom2;
				itmom2 = queryColumnFetchStats.find(query_id);
				itm2 = itmom2->second.find(key);
				if (itm2 != itmom2->second.end()) {
					itm2->second += itm->second;
					queryColumnChunkStats[query_id][key] += 1;
					queryColumnFetchDataSizeStats[query_id][key] += chunkFetchDataSizeStats_[query_id][itm->first];
			
				}
				else {
					queryColumnFetchStats[query_id][key] = itm->second;
					queryColumnChunkStats[query_id][key] = 1;
					queryColumnFetchDataSizeStats[query_id][key] = chunkFetchDataSizeStats_[query_id][itm->first];
				}
			}
		}

	
		size_t peakWorkVmSize = 0;
		if (hasPmm_) {
			peakWorkVmSize = getPeakVmSize();
			peakWorkVmSize -= GetMaxPmemBufferSize();
		}
	
		SysCatalog::instance().storeDataMgrStatistics(hasPmm_, peakWorkVmSize, query_time, queryColumnFetchStats, queryColumnChunkStats, queryColumnFetchDataSizeStats, columnFetchStats,  columnChunkStats, columnFetchDataSizeStats);

#if 0
		for (std::map<std::vector<int>, size_t>::const_iterator it2 = columnFetchStats.begin(); it2 != columnFetchStats.end(); ++it2) {
			for (std::vector<int>::const_iterator it3 = (it2->first).begin(); it3 != (it2->first).end(); it3++) {
		
				std::cout << " " << *it3;
			}
		
			std::cout << " " << it2->second;

			std::cout << " " << columnChunkStats[it2->first];
		        std::cout << " " << columnFetchDataSizeStats[it2->first];
		        std::cout << std::endl;
		}
#endif /* 0 */

		chunkFetchStats_.clear();
		chunkFetchDataSizeStats_.clear();

		statisticsOn_ = false;
	}
	chunkFetchStatsLock.unlock();

	std::cout << "Data manager statistics off." << std::endl;

	EstimateDramRequired(100);
}

size_t
DataMgr::EstimateDramRequired(int percentDramPerf)
{
	std::map<unsigned long, long> query_pmem_time;
	std::map<unsigned long, long> query_dram_time;
	std::vector<unsigned long> query_id_diff;
	std::vector<long> query_time_diff;
	std::map<unsigned long, std::map<std::vector<int>, size_t>> queryColumnFetchStats2;
	std::map<unsigned long, std::map<std::vector<int>, size_t>> queryColumnChunkStats2;
	std::map<unsigned long, std::map<std::vector<int>, size_t>> queryColumnFetchDataSizeStats2;
	std::map<std::vector<int>, size_t> columnFetchStats2;
	std::map<std::vector<int>, size_t> columnChunkStats2;
	std::map<std::vector<int>, size_t> columnFetchDataSizeStats2;

	size_t peakWorkVmSize;

	if ((percentDramPerf > 100) || (percentDramPerf < 0)) {
		std::cout << "Percentage of DRAM performance must be between 0 and 100, for example, 80." << std::endl;
		return 0;
	}

	if (SysCatalog::instance().loadDataMgrStatistics(profSF_, peakWorkVmSize, query_pmem_time, query_dram_time, query_id_diff, query_time_diff, queryColumnFetchStats2, queryColumnChunkStats2, queryColumnFetchDataSizeStats2, columnFetchStats2, columnChunkStats2, columnFetchDataSizeStats2)) {
		std::cout << "query_pmem_time and query_dram_time do not have the same query ids" << std::endl;
		return 0;
	}

	//for (unsigned int i = 0; i < query_id_diff.size(); i++) {
	//	std::cout << query_id_diff[i] << " " <<  query_time_diff[i] << std::endl;
	//}

	long query_dram_time_total;
	long query_pmem_time_total;

	query_dram_time_total = 0;
	for (std::map<unsigned long, long>::iterator it = query_dram_time.begin(); it != query_dram_time.end(); it++) {
		query_dram_time_total += it->second;
	}

	query_pmem_time_total = 0;
	for (std::map<unsigned long, long>::iterator it = query_pmem_time.begin(); it != query_pmem_time.end(); it++) {
		query_pmem_time_total += it->second;
	}

	unsigned long hotcut = 0;
	// (pmem_time - dram_time)/dram_time <= (100 - percentDramPerf)/100 ==>
	// 100 * pmem_time <= (100 * dram_time + 100 * dram_time - percentDramPerf * dram_time ==>
	// 100 * pmem_time <= (200 - percentDramPerf) * dram_time
	while (query_pmem_time_total && query_dram_time_total && ((100 * query_pmem_time_total) > ((200 - percentDramPerf) * query_dram_time_total)) && (hotcut < query_time_diff.size())) {
		query_pmem_time_total -= query_time_diff[hotcut];
		hotcut++;
	}

	std::map<std::vector<int>, size_t> hotColumnFetchStats2;
	std::map<std::vector<int>, size_t> hotColumnChunkStats2;
	std::map<std::vector<int>, size_t> hotColumnFetchDataSizeStats2;

	for (unsigned long i = 0; i < hotcut; i++) {
		unsigned long query_id;

		query_id = query_id_diff[i];
		for (std::map<std::vector<int>, size_t>::iterator it = queryColumnFetchStats2[query_id].begin(); it != queryColumnFetchStats2[query_id].end(); it++) {
			if (hotColumnFetchStats2.find(it->first) != hotColumnFetchStats2.end()) {
				hotColumnFetchStats2[it->first] += it->second;
				hotColumnChunkStats2[it->first] += queryColumnChunkStats2[query_id][it->first];
				hotColumnFetchDataSizeStats2[it->first] += queryColumnFetchDataSizeStats2[query_id][it->first];
			}
			else {
				hotColumnFetchStats2[it->first] = it->second;
				hotColumnChunkStats2[it->first] = queryColumnChunkStats2[query_id][it->first];
				hotColumnFetchDataSizeStats2[it->first] = queryColumnFetchDataSizeStats2[query_id][it->first];
			}
		}
	}


	size_t dramRequired = peakWorkVmSize;
	for (std::map<std::vector<int>, size_t>::iterator it = hotColumnFetchStats2.begin(); it != hotColumnFetchStats2.end(); it++) {
		size_t estimatedColumnSize;

		estimatedColumnSize = hotColumnFetchDataSizeStats2[it->first] * hotColumnChunkStats2[it->first] * 1.0 / it->second;
		dramRequired += estimatedColumnSize;
	}

	//printf("Need %lu DRAM\n", dramRequired);

	return dramRequired;

#if 0
	for (std::map<unsigned long, long>::const_iterator it = query_pmem_time.begin(); it != query_pmem_time.end(); ++it) {
		std::cout << it->first << " " << it->second << std::endl;
	}

	for (std::map<unsigned long, long>::const_iterator it = query_dram_time.begin(); it != query_dram_time.end(); ++it) {
		std::cout << it->first << " " << it->second << std::endl;
	}

	for (std::map<unsigned long, std::map<std::vector<int>, size_t>>::const_iterator it = queryColumnFetchStats2.begin(); it != queryColumnFetchStats2.end(); it++) {
		std::cout << it->first << std::endl;
		for (std::map<std::vector<int>, size_t>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); it2++) {
			std::cout << "		" << it2->first[0] << " " << it2->first[1] << " " << it2->first[2] << " " << it2->second << " " << queryColumnChunkStats2[it->first][it2->first] << " " << queryColumnFetchDataSizeStats2[it->first][it2->first] << std::endl;
		}
	}
#endif /* 0 */

}

DataMgr::~DataMgr() {

	//stopCollectingStatistics();

		chunkFetchStats_.clear();
		chunkFetchDataSizeStats_.clear();

  int numLevels = bufferMgrs_.size();
  for (int level = numLevels - 1; level >= 0; --level) {
    for (size_t device = 0; device < bufferMgrs_[level].size(); device++) {
      if (!hasPmm_ && level == PMM_LEVEL)
	continue;	// CPU_LEVEL and PMM_LEVEL share the same manager
      delete bufferMgrs_[level][device];
    }
  }
}

size_t DataMgr::getTotalSystemMemory() {
#ifdef __APPLE__
  int mib[2];
  size_t physical_memory;
  size_t length;
  // Get the Physical memory size
  mib[0] = CTL_HW;
  mib[1] = HW_MEMSIZE;
  length = sizeof(size_t);
  sysctl(mib, 2, &physical_memory, &length, NULL, 0);
  return physical_memory;

#else
  long pages = sysconf(_SC_PHYS_PAGES);
  long page_size = sysconf(_SC_PAGE_SIZE);
  return pages * page_size;
#endif
}

void DataMgr::populateMgrs(const MapDParameters& mapd_parameters,
                           const size_t userSpecifiedNumReaderThreads) {

  AbstractBufferMgr *pmmMgr = NULL;
  AbstractBufferMgr *cpuMgr = NULL;

  bufferMgrs_.resize(3);
  bufferMgrs_[0].push_back(new GlobalFileMgr(0, dataDir_, userSpecifiedNumReaderThreads));
  levelSizes_.push_back(1);

  size_t cpuBufferSize = mapd_parameters.cpu_buffer_mem_bytes;
  if (cpuBufferSize == 0) {  // if size is not specified
    cpuBufferSize = getTotalSystemMemory() *
                    0.8;  // should get free memory instead of this ugly heuristic
  }
  size_t cpuSlabSize = std::min(static_cast<size_t>(1L << 32), cpuBufferSize);
  // cpuSlabSize -= cpuSlabSize % 512 == 0 ? 0 : 512 - (cpuSlabSize % 512);
  cpuSlabSize = (cpuSlabSize / 512) * 512;
  LOG(INFO) << "cpuSlabSize is " << (float)cpuSlabSize / (1024 * 1024) << "M";

  //TODO: set pmmMgr
  //
  cpuMgr = new CpuBufferMgr(0, cpuBufferSize, cudaMgr_.get(), cpuSlabSize, 512, bufferMgrs_[0][0]);
  if (hasPmm_) {
	  pmmMgr = new PmmBufferMgr(0, GetMaxPmemBufferSize(), cudaMgr_.get(), cpuMgr, cpuSlabSize, 512, bufferMgrs_[0][0]);
  }
  else {
	  pmmMgr = cpuMgr;
  }

  bufferMgrs_[1].push_back(cpuMgr);
  levelSizes_.push_back(1);

  bufferMgrs_[2].push_back(pmmMgr);
  levelSizes_.push_back(1);

  if (hasGpus_) {
    LOG(INFO) << "reserved GPU memory is " << (float)reservedGpuMem_ / (1024 * 1024)
              << "M includes render buffer allocation";
    bufferMgrs_.resize(4);
    int numGpus = cudaMgr_->getDeviceCount();
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      size_t gpuMaxMemSize =
          mapd_parameters.gpu_buffer_mem_bytes != 0
              ? mapd_parameters.gpu_buffer_mem_bytes
              : (cudaMgr_->getDeviceProperties(gpuNum)->globalMem) - (reservedGpuMem_);
      size_t gpuSlabSize = std::min(static_cast<size_t>(1L << 31), gpuMaxMemSize);
      gpuSlabSize -= gpuSlabSize % 512 == 0 ? 0 : 512 - (gpuSlabSize % 512);
      LOG(INFO) << "gpuSlabSize is " << (float)gpuSlabSize / (1024 * 1024) << "M";
      bufferMgrs_[3].push_back(new GpuCudaBufferMgr(
          gpuNum, gpuMaxMemSize, cudaMgr_.get(), gpuSlabSize, 512, bufferMgrs_[1][0]));
    }
    levelSizes_.push_back(numGpus);
  }


#if 0
  bufferMgrs_.resize(2);
  bufferMgrs_[0].push_back(new GlobalFileMgr(0, dataDir_, userSpecifiedNumReaderThreads));
  levelSizes_.push_back(1);
  size_t cpuBufferSize = mapd_parameters.cpu_buffer_mem_bytes;
  if (cpuBufferSize == 0) {  // if size is not specified
    cpuBufferSize = getTotalSystemMemory() *
                    0.8;  // should get free memory instead of this ugly heuristic
  }
  size_t cpuSlabSize = std::min(static_cast<size_t>(1L << 32), cpuBufferSize);
  // cpuSlabSize -= cpuSlabSize % 512 == 0 ? 0 : 512 - (cpuSlabSize % 512);
  cpuSlabSize = (cpuSlabSize / 512) * 512;
  LOG(INFO) << "cpuSlabSize is " << (float)cpuSlabSize / (1024 * 1024) << "M";
  if (hasGpus_) {
    LOG(INFO) << "reserved GPU memory is " << (float)reservedGpuMem_ / (1024 * 1024)
              << "M includes render buffer allocation";
    bufferMgrs_.resize(3);
    bufferMgrs_[1].push_back(new CpuBufferMgr(
        0, cpuBufferSize, cudaMgr_.get(), cpuSlabSize, 512, bufferMgrs_[0][0]));
    levelSizes_.push_back(1);
    int numGpus = cudaMgr_->getDeviceCount();
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      size_t gpuMaxMemSize =
          mapd_parameters.gpu_buffer_mem_bytes != 0
              ? mapd_parameters.gpu_buffer_mem_bytes
              : (cudaMgr_->getDeviceProperties(gpuNum)->globalMem) - (reservedGpuMem_);
      size_t gpuSlabSize = std::min(static_cast<size_t>(1L << 31), gpuMaxMemSize);
      gpuSlabSize -= gpuSlabSize % 512 == 0 ? 0 : 512 - (gpuSlabSize % 512);
      LOG(INFO) << "gpuSlabSize is " << (float)gpuSlabSize / (1024 * 1024) << "M";
      bufferMgrs_[2].push_back(new GpuCudaBufferMgr(
          gpuNum, gpuMaxMemSize, cudaMgr_.get(), gpuSlabSize, 512, bufferMgrs_[1][0]));
    }
    levelSizes_.push_back(numGpus);
  } else {
    bufferMgrs_[1].push_back(new CpuBufferMgr(
        0, cpuBufferSize, cudaMgr_.get(), cpuSlabSize, 512, bufferMgrs_[0][0]));
    levelSizes_.push_back(1);
  }
#endif /* 0 */
}

void DataMgr::convertDB(const std::string basePath) {
  /* check that "mapd_data" directory exists and it's empty */
  std::string mapdDataPath(basePath + "/../mapd_data/");
  boost::filesystem::path path(mapdDataPath);
  if (boost::filesystem::exists(path)) {
    if (!boost::filesystem::is_directory(path)) {
      LOG(FATAL) << "Path to directory mapd_data to convert DB is not a directory.";
    }
  } else {  // data directory does not exist
    LOG(FATAL) << "Path to directory mapd_data to convert DB does not exist.";
  }

  GlobalFileMgr* gfm = dynamic_cast<GlobalFileMgr*>(bufferMgrs_[0][0]);
  size_t defaultPageSize = gfm->getDefaultPageSize();
  LOG(INFO) << "Database conversion started.";
  FileMgr* fm_base_db =
      new FileMgr(gfm,
                  defaultPageSize,
                  basePath);  // this call also copies data into new DB structure
  delete fm_base_db;

  /* write content of DB into newly created/converted DB structure & location */
  checkpoint();  // outputs data files as well as metadata files
  LOG(INFO) << "Database conversion completed.";
}

void DataMgr::createTopLevelMetadata()
    const {  // create metadata shared by all tables of all DBs
  ChunkKey chunkKey(2);
  chunkKey[0] = 0;  // top level db_id
  chunkKey[1] = 0;  // top level tb_id

  GlobalFileMgr* gfm = dynamic_cast<GlobalFileMgr*>(bufferMgrs_[0][0]);
  FileMgr* fm_top = gfm->getFileMgr(chunkKey);
  fm_top->createTopLevelMetadata();
}

std::vector<MemoryInfo> DataMgr::getMemoryInfo(const MemoryLevel memLevel) {
  // TODO (vraj) : Reduce the duplicate code
  // TODO: handle PMM
  std::vector<MemoryInfo> memInfo;
  if (memLevel == MemoryLevel::CPU_LEVEL) {
    CpuBufferMgr* cpuBuffer =
        dynamic_cast<CpuBufferMgr*>(bufferMgrs_[MemoryLevel::CPU_LEVEL][0]);
    MemoryInfo mi;

    mi.pageSize = cpuBuffer->getPageSize();
    mi.maxNumPages = cpuBuffer->getMaxSize() / mi.pageSize;
    mi.isAllocationCapped = cpuBuffer->isAllocationCapped();
    mi.numPageAllocated = cpuBuffer->getAllocated() / mi.pageSize;

    const std::vector<BufferList> slab_segments = cpuBuffer->getSlabSegments();
    size_t numSlabs = slab_segments.size();

    for (size_t slabNum = 0; slabNum != numSlabs; ++slabNum) {
      for (auto segIt : slab_segments[slabNum]) {
        MemoryData md;
        md.slabNum = slabNum;
        md.startPage = segIt.startPage;
        md.numPages = segIt.numPages;
        md.touch = segIt.lastTouched;
        md.isFree = segIt.memStatus;
        md.chunk_key.insert(
            md.chunk_key.end(), segIt.chunkKey.begin(), segIt.chunkKey.end());
        mi.nodeMemoryData.push_back(md);
      }
    }
    memInfo.push_back(mi);
  } else if (hasGpus_) {
    int numGpus = cudaMgr_->getDeviceCount();
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      GpuCudaBufferMgr* gpuBuffer =
          dynamic_cast<GpuCudaBufferMgr*>(bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]);
      MemoryInfo mi;

      mi.pageSize = gpuBuffer->getPageSize();
      mi.maxNumPages = gpuBuffer->getMaxSize() / mi.pageSize;
      mi.isAllocationCapped = gpuBuffer->isAllocationCapped();
      mi.numPageAllocated = gpuBuffer->getAllocated() / mi.pageSize;
      const std::vector<BufferList> slab_segments = gpuBuffer->getSlabSegments();
      size_t numSlabs = slab_segments.size();

      for (size_t slabNum = 0; slabNum != numSlabs; ++slabNum) {
        for (auto segIt : slab_segments[slabNum]) {
          MemoryData md;
          md.slabNum = slabNum;
          md.startPage = segIt.startPage;
          md.numPages = segIt.numPages;
          md.touch = segIt.lastTouched;
          md.chunk_key.insert(
              md.chunk_key.end(), segIt.chunkKey.begin(), segIt.chunkKey.end());
          md.isFree = segIt.memStatus;
          mi.nodeMemoryData.push_back(md);
        }
      }
      memInfo.push_back(mi);
    }
  }
  return memInfo;
}

/*
std::vector<MemoryData> DataMgr::getGpuMemory() {
  std::vector<MemoryData> memInfo;
  if (hasGpus_) {
    int numGpus = cudaMgr_->getDeviceCount();
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      gpuMemorySummary gms;
      gms.max = bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getMaxSize();
      gms.inUse = bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getInUseSize();
      gms.allocated = bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getAllocated();
      gms.isAllocationCapped =
bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->isAllocationCapped(); memInfo.push_back(gms);
    }
  }
  return memInfo;
}

*/
//  std::ostringstream tss;
//  size_t mb = 1024 * 1024;
//  tss << std::endl;
//  // tss << "CPU RAM TOTAL AVAILABLE   : "  std::fixed << setw(9) << setprecision(2) <<
//  // ((float)bufferMgrs_[MemoryLevel::CPU_LEVEL][0]->getMaxSize() / mb)
//  //    << std::endl;
//  tss << "CPU RAM IN BUFFER USE     : " << std::fixed << setw(9) << setprecision(2)
//      << ((float)bufferMgrs_[MemoryLevel::CPU_LEVEL][0]->getInUseSize() / mb) << " MB"
//      << std::endl;
//  if (hasGpus_) {
//    int numGpus = cudaMgr_->getDeviceCount();
//    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
//      tss << "GPU" << setfill(' ') << setw(2) << gpuNum << " RAM TOTAL AVAILABLE : " <<
//      std::fixed << setw(9)
//          << setprecision(2) <<
//          ((float)bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getMaxSize() / mb) << "
//          MB"
//          << std::endl;
//      tss << "GPU" << setfill(' ') << setw(2) << gpuNum << " RAM IN BUFFER USE   : " <<
//      std::fixed << setw(9)
//          << setprecision(2) <<
//          ((float)bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getInUseSize() / mb) << "
//          MB"
//          << std::endl;
//    }
//  }
//  return tss.str();
//}

std::string DataMgr::dumpLevel(const MemoryLevel memLevel) {
  // if gpu we need to iterate through all the buffermanagers for each card
  if (memLevel == MemoryLevel::GPU_LEVEL) {
    int numGpus = cudaMgr_->getDeviceCount();
    std::ostringstream tss;
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      tss << bufferMgrs_[memLevel][gpuNum]->printSlabs();
    }
    return tss.str();
  } else {
    return bufferMgrs_[memLevel][0]->printSlabs();
  }
}

void DataMgr::clearMemory(const MemoryLevel memLevel) {
  // if gpu we need to iterate through all the buffermanagers for each card
  if (memLevel == MemoryLevel::GPU_LEVEL) {
    if (cudaMgr_) {
      int numGpus = cudaMgr_->getDeviceCount();
      for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
        LOG(INFO) << "clear slabs on gpu " << gpuNum;
        bufferMgrs_[memLevel][gpuNum]->clearSlabs();
      }
    } else {
      throw std::runtime_error("Unable to clear GPU memory: No GPUs detected");
    }
  } else {
    if (!hasPmm_ && (memLevel == PMM_LEVEL))
      return;	// CPU_LEVEL and PMM_LEVEL share the same manager

    bufferMgrs_[memLevel][0]->clearSlabs();
  }
}

bool DataMgr::isBufferOnDevice(const ChunkKey& key,
                               const MemoryLevel memLevel,
                               const int deviceId) {
  return bufferMgrs_[memLevel][deviceId]->isBufferOnDevice(key);
}

void DataMgr::getChunkMetadataVec(
    std::vector<std::pair<ChunkKey, ChunkMetadata>>& chunkMetadataVec) {
  // Can we always assume this will just be at the disklevel bc we just
  // started?
  bufferMgrs_[0][0]->getChunkMetadataVec(chunkMetadataVec);
}

void DataMgr::getChunkMetadataVecForKeyPrefix(
    std::vector<std::pair<ChunkKey, ChunkMetadata>>& chunkMetadataVec,
    const ChunkKey& keyPrefix) {
  bufferMgrs_[0][0]->getChunkMetadataVecForKeyPrefix(chunkMetadataVec, keyPrefix);
}

AbstractBuffer* DataMgr::createChunkBuffer(const ChunkKey& key,
                                           const MemoryLevel memoryLevel,
                                           const int deviceId,
                                           const size_t page_size) {
  int level = static_cast<int>(memoryLevel);

  return bufferMgrs_[level][deviceId]->createBuffer(key, page_size);
}

AbstractBuffer* DataMgr::getChunkBuffer(const ChunkKey& key,
                                        const MemoryLevel memoryLevel,
                                        const int deviceId,
                                        const size_t numBytes,
					const unsigned long query_id) {

	//std::cout << "thread " << std::this_thread::get_id() << " getChunkBuffer for query " << query_id << " " << numBytes << std::endl;

	//for (std::vector<int>::const_iterator it2 = key.begin() ; it2 != key.end(); ++it2)
	 //    std::cout << ' ' << *it2;
	//std::cout << std::endl;

	if ((numBytes > 0) && (query_id != 0)) {
		std::unique_lock<std::mutex> chunkFetchStatsLock(chunkFetchStatsMutex_);

		if (statisticsOn_) {
			std::map<unsigned long, std::map<ChunkKey, size_t>>::iterator it;
			
			it = chunkFetchStats_.find(query_id);
			if (it == chunkFetchStats_.end()) {
				std::map<ChunkKey, size_t> queryChunkStats;
				std::map<ChunkKey, size_t> queryDataStats;

				chunkFetchStats_[query_id] = queryChunkStats;
				chunkFetchDataSizeStats_[query_id] = queryDataStats;
			}

			auto it2 = chunkFetchStats_[query_id].find(key);
			if (it2 != chunkFetchStats_[query_id].end()) {
				(it2->second)++;
				chunkFetchDataSizeStats_[query_id][key] += numBytes;
			}
			else {
				chunkFetchStats_[query_id][key] = 1;
				chunkFetchDataSizeStats_[query_id][key] = numBytes;
			}

			//std::cout << "getChunkBuffer: numBytes = " << numBytes << std::endl;
	
			//for (std::vector<int>::const_iterator it2 = key.begin() ; it2 != key.end(); ++it2)
			//     std::cout << ' ' << *it2;
	
			//std::cout << " " << chunkFetchStats_[key];
	
			//std::cout << std::endl;
		}
	
		chunkFetchStatsLock.unlock();
	}

  auto level = static_cast<size_t>(memoryLevel);

  assert(level < levelSizes_.size());     // make sure we have a legit buffermgr
  assert(deviceId < levelSizes_[level]);  // make sure we have a legit buffermgr
  return bufferMgrs_[level][deviceId]->getBuffer(key, numBytes);
}

void DataMgr::deleteChunksWithPrefix(const ChunkKey& keyPrefix) {
  int numLevels = bufferMgrs_.size();
  for (int level = numLevels - 1; level >= 0; --level) {
    for (int device = 0; device < levelSizes_[level]; ++device) {
      if (!hasPmm_ && (level == PMM_LEVEL))
	continue;	//CPU_LEVEL and PMM_LEVEL share the same manager
      bufferMgrs_[level][device]->deleteBuffersWithPrefix(keyPrefix);
    }
  }
}

// only deletes the chunks at the given memory level
void DataMgr::deleteChunksWithPrefix(const ChunkKey& keyPrefix,
                                     const MemoryLevel memLevel) {
  if (!hasPmm_ && (memLevel == PMM_LEVEL))
    return;

  if (bufferMgrs_.size() <= memLevel) {
    return;
  }
  for (int device = 0; device < levelSizes_[memLevel]; ++device) {
    bufferMgrs_[memLevel][device]->deleteBuffersWithPrefix(keyPrefix);
  }
}

AbstractBuffer* DataMgr::alloc(const MemoryLevel memoryLevel,
                               const int deviceId,
                               const size_t numBytes) {
  int level = static_cast<int>(memoryLevel);

  assert(deviceId < levelSizes_[level]);
  return bufferMgrs_[level][deviceId]->alloc(numBytes);
}

void DataMgr::free(AbstractBuffer* buffer) {
  int level = static_cast<int>(buffer->getType());

  bufferMgrs_[level][buffer->getDeviceId()]->free(buffer);
}

void DataMgr::freeAllBuffers() {
  ChunkKey keyPrefix = {-1};
  deleteChunksWithPrefix(keyPrefix);
}

void DataMgr::copy(AbstractBuffer* destBuffer, AbstractBuffer* srcBuffer) {
  destBuffer->write(srcBuffer->getMemoryPtr(),
                    srcBuffer->size(),
                    0,
                    srcBuffer->getType(),
                    srcBuffer->getDeviceId());
}

// could add function below to do arbitrary copies between buffers

// void DataMgr::copy(AbstractBuffer *destBuffer, const AbstractBuffer *srcBuffer, const
// size_t numBytes, const size_t destOffset, const size_t srcOffset) {
//} /

void DataMgr::checkpoint(const int db_id, const int tb_id) {
  for (auto levelIt = bufferMgrs_.rbegin(); levelIt != bufferMgrs_.rend(); ++levelIt) {
    // use reverse iterator so we start at GPU level, then CPU then DISK
    if (!hasPmm_ && (*levelIt == bufferMgrs_[PMM_LEVEL]))
      continue;	//CPU_LEVEL and PMM_LEVEL share the same manager
    for (auto deviceIt = levelIt->begin(); deviceIt != levelIt->end(); ++deviceIt) {
      (*deviceIt)->checkpoint(db_id, tb_id);
    }
  }
}

void DataMgr::checkpoint() {
  for (auto levelIt = bufferMgrs_.rbegin(); levelIt != bufferMgrs_.rend(); ++levelIt) {
    // use reverse iterator so we start at GPU level, then CPU then DISK
    if (!hasPmm_ && (*levelIt == bufferMgrs_[PMM_LEVEL]))
      continue;	//CPU_LEVEL and PMM_LEVEL share the same manager
    for (auto deviceIt = levelIt->begin(); deviceIt != levelIt->end(); ++deviceIt) {
      (*deviceIt)->checkpoint();
    }
  }
}

void DataMgr::removeTableRelatedDS(const int db_id, const int tb_id) {
  dynamic_cast<GlobalFileMgr*>(bufferMgrs_[0][0])->removeTableRelatedDS(db_id, tb_id);
}

void DataMgr::setTableEpoch(const int db_id, const int tb_id, const int start_epoch) {
  dynamic_cast<GlobalFileMgr*>(bufferMgrs_[0][0])
      ->setTableEpoch(db_id, tb_id, start_epoch);
}

size_t DataMgr::getTableEpoch(const int db_id, const int tb_id) {
  return dynamic_cast<GlobalFileMgr*>(bufferMgrs_[0][0])->getTableEpoch(db_id, tb_id);
}

}  // namespace Data_Namespace
