/***************************************************************************************************
 * Copyright (c) 2024 - 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************************************/



#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <math.h>
#include <vector>

///////////////////////////////////////////////////////////////////////////////////////////////////

template<typename T>
struct DeviceAllocation {
  T* ptr_ = nullptr;
  size_t offset_ = 0;
  size_t size_ = 0;

  DeviceAllocation(DeviceAllocation const&) = delete;
  DeviceAllocation& operator=(DeviceAllocation const&) = delete;

  DeviceAllocation() = default;
  DeviceAllocation(size_t size) { reset(size); }
  ~DeviceAllocation() { reset(); }

  void reset(size_t size, size_t offset=0) {
    reset();
    auto ret = cudaMalloc(&ptr_, sizeof(T) * (size + offset));
    assert(ret == cudaSuccess);
    size_ = size;
    offset_ = offset;
  }

  T* get() {
    return ptr_ + offset_;
  }

  const T* get() const {
    return ptr_ + offset_;
  }

  void reset() {
    if (ptr_ != nullptr) {
      auto ret = cudaFree(ptr_);
      assert(ret == cudaSuccess);
    }
  }

  size_t size() const { return size_; }

  size_t get_storage_size() const { return (size_ + offset_) * sizeof(T); }

  void copy_from_host(const T* ptr, size_t sz) {
    auto ret = cudaMemcpy(ptr_, ptr, sz * sizeof(T), cudaMemcpyDefault);
    assert(ret == cudaSuccess);
  }

  void copy_from_device(const T* ptr, size_t sz) {
    auto ret = cudaMemcpy(ptr_, ptr, sz * sizeof(T), cudaMemcpyDefault);
    assert(ret == cudaSuccess);
  }
};

template<typename Element>
__global__ void reference_abs_diff_kernel(
    Element* data, Element* data_ref, size_t count,
    double* max_diff, double* sum_diff,
    bool print_diff) {

    double thread_max_diff = 0;
    double thread_sum_diff = 0;

    __shared__ double block_max_diff;
    __shared__ double block_sum_diff;
    // int sum=0;
    for (size_t i = threadIdx.x + blockIdx.x * blockDim.x; i < count; i += blockDim.x * gridDim.x) {
      // if(data[i] > 0.1){printf("data[%ld]: %f, data_ref[i]: %f\n", i, (float)data[i], (float)data_ref[i]);}
      if (data[i] == data_ref[i]) {
        // if(1){
          // printf("data[%ld]: %f, data_ref[%ld]: %f，  ",i,(float)data[i],i,(float)data_ref[i]);
          // printf("row: %d, ",i/128);
        // }
        continue;
      }
      // sum++;
      // if(1){
      //   double diff = fabs(data[i] - data_ref[i]);
      //   if (print_diff) if (not isfinite(diff) || diff < 0.001f)printf("row: %d, ",i/128);
      //   // printf("difference at %lld: %f ... %f vs %f\n", static_cast<long long int>(i), diff, (double)data[i], (double)data_ref[i]);
      // }

      double diff = fabs(data[i] - data_ref[i]);
      if (print_diff) if (not isfinite(diff) || diff > 0.01f) printf("difference at %lld: %f ... %f vs %f\n", static_cast<long long int>(i), diff, (double)data[i], (double)data_ref[i]);
      thread_max_diff = fmax(diff, thread_max_diff);
      thread_sum_diff += diff;
    }

    for (int i = 0; i < blockDim.x; i++) {
      if (i == threadIdx.x) {
        if (i == 0) {
          block_max_diff = thread_max_diff;
          block_sum_diff = thread_sum_diff;
        }
        else {
          block_max_diff = fmax(block_max_diff, thread_max_diff);
          block_sum_diff += thread_sum_diff;
        }
      }
      __syncthreads();
   }

   if (threadIdx.x == 0) {
     atomicAdd(sum_diff, block_sum_diff);

     for (;;) {
       unsigned long long prev = *reinterpret_cast<unsigned long long*>(max_diff);
       double prev_diff = reinterpret_cast<double const&>(prev);
       double new_max_diff = fmax(block_max_diff, prev_diff);
       unsigned long long found = atomicCAS(reinterpret_cast<unsigned long long*>(max_diff), prev, reinterpret_cast<unsigned long long const&>(new_max_diff));
       if (found == prev) break;
    }
   }
  //  return sum;
}

template<typename Element>
void reference_abs_diff(
    DeviceAllocation<Element> const& data,
    DeviceAllocation<Element> const& data_ref,
    double& max_diff, double& mean_diff) {

  static bool kPrintDiff = getenv("REF_PRINT_DIFF") && atoi(getenv("REF_PRINT_DIFF")) == 1;
  // static bool kPrintDiff = 1;

  DeviceAllocation<double> result;
  result.reset(2);
  assert(data.size() == data_ref.size());

  cudaError_t err = cudaMemset(result.get(), 0, result.size() * sizeof(double));
  if (err != cudaSuccess) {
    std::cerr << "Memset failed. Last CUDA error: "
              << cudaGetErrorString(err) << std::endl;
    max_diff = mean_diff = 1e20;
    return;
  }

  dim3 block(256, 1, 1);
  dim3 grid(1024, 1, 1);
  // int sum1=0;
  reference_abs_diff_kernel<<<block, grid>>>(
      data.get(), data_ref.get(), data.size(),
      result.get(), result.get() + 1, kPrintDiff);
  err = cudaDeviceSynchronize();
  // printf("sum: %d\n",sum1);
  if (err != cudaSuccess) {
    std::cerr << "Difference kernel failed. Last CUDA error: "
              << cudaGetErrorString(err) << std::endl;
    max_diff = mean_diff = 1e20;
    return;
  }

  double result_host[2];
  err = cudaMemcpy(result_host, result.get(), result.size() * sizeof(double), cudaMemcpyDefault);
  if (err != cudaSuccess) {
    std::cerr << "Copy failed. Last CUDA error: "
              << cudaGetErrorString(err) << std::endl;
    max_diff = mean_diff = 1e20;
    return;
  }

  max_diff = result_host[0];
  mean_diff = result_host[1] / static_cast<double>(data.size());
}

template<typename Element>
__global__ void reference_rel_diff_kernel(
    Element* data, Element* data_ref, size_t count,
    double* max_diff, double* sum_diff,
    bool print_diff ) {

    double thread_max_diff = 0;
    double thread_sum_diff = 0;

    __shared__ double block_max_diff;
    __shared__ double block_sum_diff;

    for (size_t i = threadIdx.x + blockIdx.x * blockDim.x; i < count; i += blockDim.x * gridDim.x) {
      if (data[i] == data_ref[i]) {
        continue;
      }
      double diff = fabs(data[i] - data_ref[i]) / fabs(data_ref[i]);
      if (print_diff) if (not isfinite(diff) || diff < 0.01f) printf("difference at %lld: %f ... %f vs %f\n", static_cast<long long int>(i), diff, (double)data[i], (double)data_ref[i]);
      thread_max_diff = fmax(diff, thread_max_diff);
      thread_sum_diff += diff;
    }

    for (int i = 0; i < blockDim.x; i++) {
      if (i == threadIdx.x) {
        if (i == 0) {
          block_max_diff = thread_max_diff;
          block_sum_diff = thread_sum_diff;
        }
        else {
          block_max_diff = fmax(block_max_diff, thread_max_diff);
          block_sum_diff += thread_sum_diff;
        }
      }
      __syncthreads();
   }

   if (threadIdx.x == 0) {
     atomicAdd(sum_diff, block_sum_diff);

     for (;;) {
       unsigned long long prev = *reinterpret_cast<unsigned long long*>(max_diff);
       double prev_diff = reinterpret_cast<double const&>(prev);
       double new_max_diff = fmax(block_max_diff, prev_diff);
       unsigned long long found = atomicCAS(reinterpret_cast<unsigned long long*>(max_diff), prev, reinterpret_cast<unsigned long long const&>(new_max_diff));
       if (found == prev) break;
    }
   }
}

template<typename Element>
void reference_rel_diff(
    DeviceAllocation<Element> const& data,
    DeviceAllocation<Element> const& data_ref,
    double& max_diff, double& mean_diff) {

  static bool kPrintDiff = getenv("REF_PRINT_DIFF") && atoi(getenv("REF_PRINT_DIFF")) == 1;

  DeviceAllocation<double> result;
  result.reset(2);
  assert(data.size() == data_ref.size());

  cudaError_t err = cudaMemset(result.get(), 0, result.size() * sizeof(double));
  if (err != cudaSuccess) {
    std::cerr << "Memset failed. Last CUDA error: "
              << cudaGetErrorString(err) << std::endl;
    max_diff = mean_diff = 1e20;
    return;
  }

  dim3 block(256, 1, 1);
  dim3 grid(1024, 1, 1);
  reference_rel_diff_kernel<<<block, grid>>>(
      data.get(), data_ref.get(), data.size(),
      result.get(), result.get() + 1, kPrintDiff);

  err = cudaDeviceSynchronize();
  if (err != cudaSuccess) {
    std::cerr << "Difference kernel failed. Last CUDA error: "
              << cudaGetErrorString(err) << std::endl;
    max_diff = mean_diff = 1e20;
    return;
  }

  double result_host[2];
  err = cudaMemcpy(result_host, result.get(), result.size() * sizeof(double), cudaMemcpyDefault);
  if (err != cudaSuccess) {
    std::cerr << "Copy failed. Last CUDA error: "
              << cudaGetErrorString(err) << std::endl;
    max_diff = mean_diff = 1e20;
    return;
  }

  max_diff = result_host[0];
  mean_diff = result_host[1] / static_cast<double>(data.size());
}

template<typename Element>
void reference_rel_diff_diagnostics(
    DeviceAllocation<Element> const& data,
    DeviceAllocation<Element> const& data_ref,
    int q_extent,
    int d_extent,
    int h_extent,
    int b_extent,
    int top_k = 8) {

  assert(data.size() == data_ref.size());

  std::vector<Element> host_data(data.size());
  std::vector<Element> host_ref(data_ref.size());

  cudaError_t err = cudaMemcpy(
      host_data.data(), data.get(), data.size() * sizeof(Element), cudaMemcpyDefault);
  if (err != cudaSuccess) {
    std::cerr << "Diagnostic copy for output failed. Last CUDA error: "
              << cudaGetErrorString(err) << std::endl;
    return;
  }

  err = cudaMemcpy(
      host_ref.data(), data_ref.get(), data_ref.size() * sizeof(Element), cudaMemcpyDefault);
  if (err != cudaSuccess) {
    std::cerr << "Diagnostic copy for reference failed. Last CUDA error: "
              << cudaGetErrorString(err) << std::endl;
    return;
  }

  struct Bucket {
    const char* name;
    double max_rel = 0.0;
    double sum_rel = 0.0;
    double max_abs = 0.0;
    size_t count = 0;
    size_t nonfinite = 0;
  };

  std::array<Bucket, 3> buckets{{
      {"|ref| < 1e-3"},
      {"1e-3 <= |ref| < 1e-2"},
      {"|ref| >= 1e-2"}}};

  struct TopEntry {
    double rel = -1.0;
    double abs = 0.0;
    double out = 0.0;
    double ref = 0.0;
    size_t index = 0;
  };

  std::vector<TopEntry> top;
  top.reserve(std::max(top_k, 0));

  auto push_top = [&](TopEntry entry) {
    if (top_k <= 0) {
      return;
    }
    if (int(top.size()) < top_k) {
      top.push_back(entry);
      std::push_heap(top.begin(), top.end(), [](TopEntry const& a, TopEntry const& b) {
        return a.rel > b.rel;
      });
    }
    else if (entry.rel > top.front().rel) {
      std::pop_heap(top.begin(), top.end(), [](TopEntry const& a, TopEntry const& b) {
        return a.rel > b.rel;
      });
      top.back() = entry;
      std::push_heap(top.begin(), top.end(), [](TopEntry const& a, TopEntry const& b) {
        return a.rel > b.rel;
      });
    }
  };

  for (size_t i = 0; i < host_data.size(); ++i) {
    double out = static_cast<double>(host_data[i]);
    double ref = static_cast<double>(host_ref[i]);
    double abs_diff = fabs(out - ref);
    double ref_abs = fabs(ref);
    double rel = ref_abs == 0.0
        ? (abs_diff == 0.0 ? 0.0 : std::numeric_limits<double>::infinity())
        : abs_diff / ref_abs;

    int bucket_idx = ref_abs < 1e-3 ? 0 : (ref_abs < 1e-2 ? 1 : 2);
    auto& bucket = buckets[bucket_idx];
    bucket.count += 1;
    bucket.max_abs = std::max(bucket.max_abs, abs_diff);
    if (std::isfinite(rel)) {
      bucket.max_rel = std::max(bucket.max_rel, rel);
      bucket.sum_rel += rel;
    }
    else {
      bucket.nonfinite += 1;
      bucket.max_rel = std::numeric_limits<double>::infinity();
    }

    push_top(TopEntry{rel, abs_diff, out, ref, i});
  }

  std::sort(top.begin(), top.end(), [](TopEntry const& a, TopEntry const& b) {
    return a.rel > b.rel;
  });

  std::cout << "O relative error buckets:\n";
  for (auto const& bucket : buckets) {
    double mean_rel = bucket.count == 0 ? 0.0 : bucket.sum_rel / double(bucket.count);
    std::cout << "  " << bucket.name
              << " count=" << bucket.count
              << " max_rel=" << bucket.max_rel
              << " mean_rel=" << mean_rel
              << " max_abs=" << bucket.max_abs
              << " nonfinite=" << bucket.nonfinite
              << "\n";
  }

  int hb_extent = h_extent * b_extent;
  int row_stride = std::max(d_extent * h_extent, 1);
  int batch_stride = std::max(q_extent * row_stride, 1);

  std::cout << "Top relative O differences:\n";
  for (auto const& entry : top) {
    size_t rem = entry.index;
    int b = hb_extent == 0 ? 0 : int(rem / batch_stride);
    rem = hb_extent == 0 ? rem : rem % batch_stride;
    int q = row_stride == 0 ? 0 : int(rem / row_stride);
    rem = row_stride == 0 ? rem : rem % row_stride;
    int h = d_extent == 0 ? 0 : int(rem / d_extent);
    int d = d_extent == 0 ? 0 : int(rem % d_extent);
    std::cout << "  idx=" << entry.index
              << " q=" << q
              << " d=" << d
              << " h=" << h
              << " b=" << b
              << " rel=" << entry.rel
              << " abs=" << entry.abs
              << " out=" << entry.out
              << " ref=" << entry.ref
              << "\n";
  }
}
