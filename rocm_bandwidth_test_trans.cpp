////////////////////////////////////////////////////////////////////////////////
//
// The University of Illinois/NCSA
// Open Source License (NCSA)
// 
// Copyright (c) 2014-2015, Advanced Micro Devices, Inc. All rights reserved.
// 
// Developed by:
// 
//                 AMD Research and AMD HSA Software Development
// 
//                 Advanced Micro Devices, Inc.
// 
//                 www.amd.com
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// 
//  - Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimers.
//  - Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimers in
//    the documentation and/or other materials provided with the distribution.
//  - Neither the names of Advanced Micro Devices, Inc,
//    nor the names of its contributors may be used to endorse or promote
//    products derived from this Software without specific prior written
//    permission.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS WITH THE SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#include "common.hpp"
#include "rocm_bandwidth_test.hpp"

bool RocmBandwidthTest::BuildReadOrWriteTrans(uint32_t req_type,
                                      vector<size_t>& in_list) {

  // Validate the list of pool-agent tuples
  hsa_status_t status;
  hsa_amd_memory_pool_access_t access;
  uint32_t list_size = in_list.size();
  for (uint32_t idx = 0; idx < list_size; idx+=2) {

    uint32_t pool_idx = in_list[idx];
    uint32_t exec_idx = in_list[idx + 1];

    // Retrieve Roc runtime handles for memory pool and agent
    hsa_agent_t exec_agent = agent_list_[exec_idx].agent_;
    hsa_amd_memory_pool_t pool = pool_list_[pool_idx].pool_;

    // Determine agent can access the memory pool
    status = hsa_amd_agent_memory_pool_get_info(exec_agent, pool,
                           HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access);
    ErrorCheck(status);

    // Determine if accessibility to agent is not denied
    if (access == HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED) {
      PrintIOAccessError(exec_idx, pool_idx);
      return false;
    }

    // Agent has access, build an instance of transaction
    // and add it to the list of transactions
    async_trans_t trans(req_type);
    trans.kernel.code_ = NULL;
    trans.kernel.pool_ = pool;
    trans.kernel.pool_idx_ = pool_idx;
    trans.kernel.agent_ = exec_agent;
    trans.kernel.agent_idx_ = exec_idx;
    trans_list_.push_back(trans);
  }
  return true;
}

bool RocmBandwidthTest::BuildReadTrans() {
  return BuildReadOrWriteTrans(REQ_READ, read_list_);
}

bool RocmBandwidthTest::BuildWriteTrans() {
  return BuildReadOrWriteTrans(REQ_WRITE, write_list_);
}

bool RocmBandwidthTest::FilterCpuPool(uint32_t req_type,
                              hsa_device_type_t dev_type,
                              bool fine_grained) {

  if ((req_type != REQ_COPY_ALL_BIDIR) &&
      (req_type != REQ_COPY_ALL_UNIDIR)) {
    return false;
  }

  // Determine if device is a Cpu - filter out only if
  // it is a Cpu device
  if (dev_type != HSA_DEVICE_TYPE_CPU) {
    return false;
  }

  // If env to skip fine grain is NULL it means
  // we should filter out coarse-grain pools
  if (skip_fine_grain_ == NULL) {
    return (fine_grained == false);
  }

  // If env to skip fine grain is NON-NULL it means
  // we should filter out fine-grain pools
  return (fine_grained == true);
}

bool RocmBandwidthTest::BuildCopyTrans(uint32_t req_type,
                               vector<size_t>& src_list,
                               vector<size_t>& dst_list) {

  // bool filter_out;
  uint32_t src_size = src_list.size();
  uint32_t dst_size = dst_list.size();

  for (uint32_t idx = 0; idx < src_size; idx++) {

    // Retrieve Roc runtime handles for Src memory pool and agents
    uint32_t src_idx = src_list[idx];
    uint32_t src_dev_idx = pool_list_[src_idx].agent_index_;
    hsa_amd_memory_pool_t src_pool = pool_list_[src_idx].pool_;
    hsa_device_type_t src_dev_type = agent_list_[src_dev_idx].device_type_;

    for (uint32_t jdx = 0; jdx < dst_size; jdx++) {

      // Retrieve Roc runtime handles for Dst memory pool and agents
      uint32_t dst_idx = dst_list[jdx];
      uint32_t dst_dev_idx = pool_list_[dst_idx].agent_index_;
      hsa_amd_memory_pool_t dst_pool = pool_list_[dst_idx].pool_;
      hsa_device_type_t dst_dev_type = agent_list_[dst_dev_idx].device_type_;

      // Filter out transactions that involve only Cpu agents/devices
      // without regard to type of request, default run, partial or full
      // unidirectional or bidirectional copies
      if ((src_dev_type == HSA_DEVICE_TYPE_CPU) &&
          (dst_dev_type == HSA_DEVICE_TYPE_CPU)) {
        continue;
      }

      // Filter out transactions that involve only same GPU as both
      // Src and Dst device if the request is bidirectional copy that
      // is either partial or full
      if ((req_type == REQ_COPY_BIDIR) ||
          (req_type == REQ_COPY_ALL_BIDIR)) {
        if (src_dev_idx == dst_dev_idx) {
          continue;
        }
      }

      // Determine if accessibility to dst pool for src agent is not denied
      uint32_t path_exists = access_matrix_[(src_dev_idx * agent_index_) + dst_dev_idx];
      if (path_exists == 0) {
        if ((req_type == REQ_COPY_ALL_BIDIR) ||
            (req_type == REQ_COPY_ALL_UNIDIR)) {
          continue;
        } else {
          PrintCopyAccessError(src_idx, dst_idx);
          return false;
        }
      }

      // Update the list of agents active in any copy operation
      if (active_agents_list_ == NULL) {
        active_agents_list_  = new uint32_t[agent_index_]();
      }
      active_agents_list_[src_dev_idx] = 1;
      active_agents_list_[dst_dev_idx] = 1;

      // Agents have access, build an instance of transaction
      // and add it to the list of transactions
      async_trans_t trans(req_type);
      trans.copy.src_idx_ = src_idx;
      trans.copy.dst_idx_ = dst_idx;
      trans.copy.src_pool_ = src_pool;
      trans.copy.dst_pool_ = dst_pool;
      trans.copy.bidir_ = ((req_type == REQ_COPY_BIDIR) ||
                           (req_type == REQ_COPY_ALL_BIDIR));
      trans.copy.uses_gpu_ = ((src_dev_type == HSA_DEVICE_TYPE_GPU) ||
                              (dst_dev_type == HSA_DEVICE_TYPE_GPU));
      trans_list_.push_back(trans);
    }
  }

  return true;
}

bool RocmBandwidthTest::BuildBidirCopyTrans() {
  return BuildCopyTrans(REQ_COPY_BIDIR, bidir_list_, bidir_list_);
}

bool RocmBandwidthTest::BuildUnidirCopyTrans() {
  return BuildCopyTrans(REQ_COPY_UNIDIR, src_list_, dst_list_);
}

bool RocmBandwidthTest::BuildAllPoolsBidirCopyTrans() {
  return BuildCopyTrans(REQ_COPY_ALL_BIDIR, bidir_list_, bidir_list_);
}

bool RocmBandwidthTest::BuildAllPoolsUnidirCopyTrans() {
  return BuildCopyTrans(REQ_COPY_ALL_UNIDIR, src_list_, dst_list_);
}

// @brief: Builds a list of transaction per user request
bool RocmBandwidthTest::BuildTransList() {

  // Build list of Read transactions per user request
  bool status = false;
  if (req_read_ == REQ_READ) {
    status = BuildReadTrans();
    if (status == false) {
      return status;
    }
  }

  // Build list of Write transactions per user request
  status = false;
  if (req_write_ == REQ_WRITE) {
    status = BuildWriteTrans();
    if (status == false) {
      return status;
    }
  }

  // Build list of Bidirectional Copy transactions per user request
  status = false;
  if (req_copy_bidir_ == REQ_COPY_BIDIR) {
    status = BuildBidirCopyTrans();
    if (status == false) {
      return status;
    }
  }

  // Build list of Unidirectional Copy transactions per user request
  status = false;
  if (req_copy_unidir_ == REQ_COPY_UNIDIR) {
    status = BuildUnidirCopyTrans();
    if (status == false) {
      return status;
    }
  }

  // Build list of All Bidir Copy transactions per user request
  status = false;
  if (req_copy_all_bidir_ == REQ_COPY_ALL_BIDIR) {
    status = BuildAllPoolsBidirCopyTrans();
    if (status == false) {
      return status;
    }
  }

  // Build list of All Unidir Copy transactions per user request
  status = false;
  if (req_copy_all_unidir_ == REQ_COPY_ALL_UNIDIR) {
    status = BuildAllPoolsUnidirCopyTrans();
    if (status == false) {
      return status;
    }
  }

  // All of the transaction are built up
  return true;
}

void RocmBandwidthTest::ComputeCopyTime(async_trans_t& trans) {

  // Get the frequency of Gpu Timestamping
  uint64_t sys_freq = 0;
  hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP_FREQUENCY, &sys_freq);

  double avg_time = 0;
  double min_time = 0;
  size_t data_size = 0;
  double avg_bandwidth = 0;
  double peak_bandwidth = 0;
  uint32_t size_len = size_list_.size();
  for (uint32_t idx = 0; idx < size_len; idx++) {

    // Adjust size of data involved in copy
    data_size = size_list_[idx];
    if (trans.copy.bidir_ == true) {
      data_size += size_list_[idx];
    }

    // Double data size if copying the same device
    if (trans.copy.src_idx_ == trans.copy.dst_idx_) {
      data_size += data_size;
    }

    // Copy operation does not involve a Gpu device
    // Divide bandwidth with 10^9 to get size in GigaBytes (10^9)
    if (trans.copy.uses_gpu_ != true) {
      avg_time = trans.cpu_avg_time_[idx];
      min_time = trans.cpu_min_time_[idx];
      avg_bandwidth = (double)data_size / avg_time / 1000 / 1000 / 1000;
      peak_bandwidth = (double)data_size / min_time / 1000 / 1000 / 1000;
    } else {
      if (print_cpu_time_ == false) {
        avg_time = trans.gpu_avg_time_[idx] / sys_freq;
        min_time = trans.gpu_min_time_[idx] / sys_freq;
      } else {
        avg_time = trans.cpu_avg_time_[idx];
        min_time = trans.cpu_min_time_[idx];
      }
      avg_bandwidth = (double)data_size / avg_time / 1000 / 1000 / 1000;
      peak_bandwidth = (double)data_size / min_time / 1000 / 1000 / 1000;
    }

    trans.min_time_.push_back(min_time);
    trans.avg_time_.push_back(avg_time);
    trans.avg_bandwidth_.push_back(avg_bandwidth);
    trans.peak_bandwidth_.push_back(peak_bandwidth);
  }
}

