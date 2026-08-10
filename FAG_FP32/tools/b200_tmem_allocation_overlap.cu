// Measure how many CTA-local TMEM allocations can be held concurrently on one
// Blackwell SM.  The kernel intentionally uses very few registers and no
// dynamic shared memory so CUDA launch resources do not cap CTA residency.
#include <cuda_runtime.h>

#include <cute/arch/tmem_allocator_sm100.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

struct Record {
  unsigned long long attempt_ns;
  unsigned long long acquired_ns;
  unsigned long long release_ns;
  unsigned int smid;
  unsigned int block;
  unsigned int tmem_base;
};

inline void check(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    std::fprintf(stderr, "%s: %s\n", what, cudaGetErrorString(status));
    std::exit(EXIT_FAILURE);
  }
}

__device__ __forceinline__ unsigned int read_smid() {
  unsigned int smid;
  asm volatile("mov.u32 %0, %%smid;" : "=r"(smid));
  return smid;
}

__device__ __forceinline__ unsigned long long read_globaltimer() {
  unsigned long long value;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(value));
  return value;
}

__global__ void hold_tmem_allocation(Record* records, int columns,
                                     unsigned long long hold_cycles) {
  __shared__ unsigned int tmem_base;
  const int lane = threadIdx.x & 31;

  const unsigned long long attempt = read_globaltimer();
  cute::TMEM::Allocator1Sm allocator;
  allocator.allocate(columns, &tmem_base);
  __syncwarp();
  const unsigned long long acquired = read_globaltimer();

  // Match the normal CUTLASS lifetime: release the right to make another
  // allocation, but retain ownership of the columns until deallocation.
  allocator.release_allocation_lock();
  __syncwarp();

  const unsigned long long start_clock = clock64();
  while (clock64() - start_clock < hold_cycles) {
    asm volatile("" ::: "memory");
  }
  __syncwarp();

  const unsigned long long release = read_globaltimer();
  if (lane == 0) {
    records[blockIdx.x] =
        Record{attempt, acquired, release, read_smid(), blockIdx.x, tmem_base};
  }
  __syncwarp();
  allocator.free(tmem_base, columns);
}

// Exact allocation mode used by the target FMHA kernel: two peer CTAs issue
// the collective allocation/deallocation as one cta_group::2 operation.
__global__ void __cluster_dims__(2, 1, 1)
hold_tmem_allocation_2sm(Record* records, int columns,
                         unsigned long long hold_cycles) {
  __shared__ unsigned int tmem_base;
  const int lane = threadIdx.x & 31;

  const unsigned long long attempt = read_globaltimer();
  cute::TMEM::Allocator2Sm allocator;
  allocator.allocate(columns, &tmem_base);
  __syncwarp();
  const unsigned long long acquired = read_globaltimer();

  allocator.release_allocation_lock();
  __syncwarp();

  const unsigned long long start_clock = clock64();
  while (clock64() - start_clock < hold_cycles) {
    asm volatile("" ::: "memory");
  }
  __syncwarp();

  const unsigned long long release = read_globaltimer();
  if (lane == 0) {
    records[blockIdx.x] =
        Record{attempt, acquired, release, read_smid(), blockIdx.x, tmem_base};
  }
  __syncwarp();
  allocator.free(tmem_base, columns);
}

struct Event {
  unsigned long long time;
  int delta;
};

void run_case(int columns, int blocks, int sm_count, Record* d_records) {
  constexpr unsigned long long kHoldCycles = 100000;
  std::vector<Record> records(blocks);

  hold_tmem_allocation<<<blocks, 32>>>(d_records, columns, kHoldCycles);
  check(cudaGetLastError(), "launch hold_tmem_allocation");
  check(cudaMemcpy(records.data(), d_records, sizeof(Record) * blocks,
                   cudaMemcpyDeviceToHost),
        "copy records");

  std::map<unsigned int, std::vector<Record>> by_sm;
  std::vector<double> waits;
  for (const Record& record : records) {
    by_sm[record.smid].push_back(record);
    waits.push_back(double(record.acquired_ns - record.attempt_ns));
  }

  std::vector<int> max_concurrency;
  int overlap_pairs = 0;
  for (auto& [smid, sm_records] : by_sm) {
    std::vector<Event> events;
    for (const Record& record : sm_records) {
      // End events sort before start events at equal timestamps.
      events.push_back(Event{record.acquired_ns, +1});
      events.push_back(Event{record.release_ns, -1});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
      return a.time != b.time ? a.time < b.time : a.delta < b.delta;
    });
    int active = 0;
    int maximum = 0;
    for (const Event& event : events) {
      active += event.delta;
      maximum = std::max(maximum, active);
    }
    max_concurrency.push_back(maximum);

    std::sort(sm_records.begin(), sm_records.end(), [](const Record& a,
                                                       const Record& b) {
      return a.acquired_ns < b.acquired_ns;
    });
    for (size_t i = 1; i < sm_records.size(); ++i) {
      if (sm_records[i].acquired_ns < sm_records[i - 1].release_ns) {
        ++overlap_pairs;
      }
    }
  }

  std::sort(waits.begin(), waits.end());
  std::sort(max_concurrency.begin(), max_concurrency.end());
  const auto median_wait = waits[waits.size() / 2];
  const int min_concurrency = max_concurrency.front();
  const int median_concurrency = max_concurrency[max_concurrency.size() / 2];
  const int max_concurrent = max_concurrency.back();

  std::printf(
      "columns=%3d blocks=%4d observed_sms=%3zu/%d "
      "max_concurrency[min/median/max]=%d/%d/%d overlap_pairs=%d "
      "median_alloc_wait_ns=%.0f\n",
      columns, blocks, by_sm.size(), sm_count, min_concurrency,
      median_concurrency, max_concurrent, overlap_pairs, median_wait);
}

void run_case_2sm(int columns, int blocks, int sm_count, Record* d_records) {
  constexpr unsigned long long kHoldCycles = 100000;
  std::vector<Record> records(blocks);

  hold_tmem_allocation_2sm<<<blocks, 32>>>(d_records, columns, kHoldCycles);
  check(cudaGetLastError(), "launch hold_tmem_allocation_2sm");
  check(cudaMemcpy(records.data(), d_records, sizeof(Record) * blocks,
                   cudaMemcpyDeviceToHost),
        "copy 2SM records");

  std::map<unsigned int, std::vector<Record>> by_sm;
  std::vector<double> waits;
  for (const Record& record : records) {
    by_sm[record.smid].push_back(record);
    waits.push_back(double(record.acquired_ns - record.attempt_ns));
  }

  std::vector<int> max_concurrency;
  int overlap_pairs = 0;
  for (auto& [smid, sm_records] : by_sm) {
    std::vector<Event> events;
    for (const Record& record : sm_records) {
      events.push_back(Event{record.acquired_ns, +1});
      events.push_back(Event{record.release_ns, -1});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
      return a.time != b.time ? a.time < b.time : a.delta < b.delta;
    });
    int active = 0;
    int maximum = 0;
    for (const Event& event : events) {
      active += event.delta;
      maximum = std::max(maximum, active);
    }
    max_concurrency.push_back(maximum);

    std::sort(sm_records.begin(), sm_records.end(), [](const Record& a,
                                                       const Record& b) {
      return a.acquired_ns < b.acquired_ns;
    });
    for (size_t i = 1; i < sm_records.size(); ++i) {
      if (sm_records[i].acquired_ns < sm_records[i - 1].release_ns) {
        ++overlap_pairs;
      }
    }
  }

  std::sort(waits.begin(), waits.end());
  std::sort(max_concurrency.begin(), max_concurrency.end());
  std::printf(
      "cta_group::2 columns=%3d blocks=%4d observed_sms=%3zu/%d "
      "max_concurrency[min/median/max]=%d/%d/%d overlap_pairs=%d "
      "median_alloc_wait_ns=%.0f\n",
      columns, blocks, by_sm.size(), sm_count, max_concurrency.front(),
      max_concurrency[max_concurrency.size() / 2], max_concurrency.back(),
      overlap_pairs, waits[waits.size() / 2]);
}

}  // namespace

int main() {
  cudaDeviceProp prop{};
  check(cudaGetDeviceProperties(&prop, 0), "cudaGetDeviceProperties");
  if (prop.major != 10) {
    std::fprintf(stderr, "This experiment requires an SM100-class GPU.\n");
    return EXIT_FAILURE;
  }

  int occupancy = 0;
  check(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
            &occupancy, hold_tmem_allocation, 32, 0),
        "cudaOccupancyMaxActiveBlocksPerMultiprocessor");
  const int blocks = prop.multiProcessorCount * 8;
  std::printf("GPU=%s SMs=%d launch-resource block_limit/SM=%d\n", prop.name,
              prop.multiProcessorCount, occupancy);

  Record* d_records = nullptr;
  check(cudaMalloc(&d_records, sizeof(Record) * blocks), "cudaMalloc records");

  // Warm the context and instruction path outside the measured cases.
  hold_tmem_allocation<<<1, 32>>>(d_records, 32, 1000);
  check(cudaDeviceSynchronize(), "warmup");

  for (int columns : {512, 256, 128}) {
    run_case(columns, blocks, prop.multiProcessorCount, d_records);
  }
  run_case_2sm(512, blocks, prop.multiProcessorCount, d_records);

  check(cudaFree(d_records), "cudaFree records");
  return EXIT_SUCCESS;
}
