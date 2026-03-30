# High-Performance-Branchless-Tokenizer

A high-performance C++ tokenizer designed for large-scale text processing on multicore NUMA systems. This project uses a branchless character classification approach, multithreaded file loading and processing, and optional NUMA-aware thread affinity to improve throughput when scanning large datasets.

## Overview

This project reads text files from a directory, loads them into memory, tokenizes them using a branchless lookup-table-based tokenizer, and reports performance statistics such as:

- total bytes processed
- total tokens discovered
- per-thread tokenization time
- per-thread bytes processed
- overall throughput in MB/s

The system is designed to explore performance-oriented techniques in C++, including:

- branchless tokenization
- multithreading
- NUMA-aware thread placement
- queue-based producer/consumer style processing
- large-scale file scanning and benchmarking

## Features

- **Branchless tokenizer**
  - Uses a 256-entry lookup table to classify characters as token or delimiter characters
  - Replaces delimiters with `'\0'`
  - Returns pointers to token starts without building new strings

- **Multithreaded file loading**
  - Files are scanned and distributed across NUMA nodes
  - Loader threads read files into memory before processing begins

- **Multithreaded tokenization**
  - Worker threads pull loaded files from per-node queues
  - Each worker tokenizes files and updates shared statistics

- **Optional NUMA affinity**
  - Threads can be pinned to CPUs belonging to a specific NUMA node
  - Useful for studying locality and memory-aware performance behavior

- **Performance reporting**
  - Reports per-thread timing and bytes processed
  - Reports total throughput in MB/s

## How It Works

The program runs in two major stages:

### 1. Dataset discovery and loading
- Recursively scans the input directory
- Collects file paths and file sizes
- Sorts files by size
- Distributes files across NUMA nodes
- Launches one loader thread per NUMA node
- Each loader reads its assigned files into memory and pushes them into a per-node queue

### 2. Parallel tokenization
- Launches worker threads
- Each worker is assigned to a NUMA node in round-robin order
- Workers pop files from the queue for that node
- Each file is tokenized using the branchless tokenizer
- Shared counters are updated for total bytes and total tokens
- Per-thread timing and byte statistics are recorded

## Branchless Tokenization Strategy

The tokenizer uses a lookup table of 256 entries:

- alphanumeric characters are marked as token characters
- non-alphanumeric characters are marked as delimiters

During tokenization:
- each byte is masked using the lookup table
- delimiters become `'\0'`
- token starts are detected when the current character is valid and the previous character was a delimiter

This avoids repeated branching on character classification and keeps the inner loop simple and fast.

## Build Requirements

- C++17 or later
- POSIX system (Linux recommended)
- NUMA library (`libnuma`)
- pthread support

## Build

Example using CMake:

```bash
mkdir build
cd build
cmake ..
make
```

## Benchmark Results

All experiments below were run on a dual-socket **Intel Xeon Gold 6240R (Cascade Lake)** server using the **Gutenberg 110 GB dataset** and additional **30 GB logs** experiments for low-level hardware-counter analysis.

### Hardware Platform

- **CPU:** Intel Xeon Gold 6240R @ 2.40 GHz
- **Architecture:** x86_64
- **Sockets:** 2
- **Cores per socket:** 24
- **Threads per core:** 2
- **Total logical CPUs:** 96
- **NUMA nodes:** 2

---

## Throughput Benchmark: Gutenberg 110 GB Dataset

The following results compare three tokenization approaches:

- **REGEX**
- **STRTOK**
- **Branchless**

Each method was tested with and without NUMA affinity across multiple thread counts.

### REGEX — NonAffinity

| Threads | 1 | 2 | 4 | 8 | 16 | 32 | 48 | 64 | 96 |
|--------:|--:|--:|--:|--:|---:|---:|---:|---:|---:|
| Throughput (MB/s) | 19.9011 | 39.6893 | 78.5326 | 159.8589 | 314.1018 | 622.2580 | 864.2564 | 867.6231 | 869.5535 |

### REGEX — Affinity

| Threads | 1 | 2 | 4 | 8 | 16 | 32 | 48 | 64 | 96 |
|--------:|--:|--:|--:|--:|---:|---:|---:|---:|---:|
| Throughput (MB/s) | 19.8468 | 39.8784 | 80.2305 | 160.0755 | 319.3094 | 633.5610 | 893.6572 | 889.7335 | 868.2643 |

### STRTOK — NonAffinity

| Threads | 1 | 2 | 4 | 8 | 16 | 32 | 48 | 64 | 96 |
|--------:|--:|--:|--:|--:|---:|---:|---:|---:|---:|
| Throughput (MB/s) | 25.7024 | 50.6941 | 101.8495 | 202.4024 | 408.9888 | 802.9678 | 1109.0178 | 1137.3709 | 1140.2856 |

### STRTOK — Affinity

| Threads | 1 | 2 | 4 | 8 | 16 | 32 | 48 | 64 | 96 |
|--------:|--:|--:|--:|--:|---:|---:|---:|---:|---:|
| Throughput (MB/s) | 25.6316 | 51.2318 | 103.0257 | 205.8572 | 413.1728 | 820.7594 | 1166.8293 | 1149.4979 | 1137.3868 |

### Branchless — NonAffinity

| Threads | 1 | 2 | 4 | 8 | 16 | 32 | 48 | 64 | 96 |
|--------:|--:|--:|--:|--:|---:|---:|---:|---:|---:|
| Throughput (MB/s) | 307.5535 | 529.8167 | 994.3426 | 1848.3038 | 3826.4668 | 6869.5108 | 9195.7505 | 11160.4929 | 10646.2115 |

### Branchless — Affinity

| Threads | 1 | 2 | 4 | 8 | 16 | 32 | 48 | 64 | 96 |
|--------:|--:|--:|--:|--:|---:|---:|---:|---:|---:|
| Throughput (MB/s) | 308.7347 | 585.1260 | 1180.3051 | 2413.8687 | 4754.8505 | 9077.5933 | 12447.0489 | 14239.8721 | 15900.4472 |

---

## Key Throughput Observations

- The **branchless tokenizer** dramatically outperforms both REGEX and STRTOK at every thread count.
- At **1 thread**, branchless reaches **308.7347 MB/s** with affinity, compared with:
  - **19.8468 MB/s** for REGEX
  - **25.6316 MB/s** for STRTOK
- At **96 threads**, branchless with affinity reaches **15900.4472 MB/s**, compared with:
  - **868.2643 MB/s** for REGEX
  - **1137.3868 MB/s** for STRTOK
- NUMA-aware affinity generally improves branchless performance, especially at higher thread counts.

### Relative Speedup at 96 Threads (Affinity)

Compared to REGEX:
- **Branchless is about 18.3× faster**  
  (`15900.4472 / 868.2643 ≈ 18.31`)

Compared to STRTOK:
- **Branchless is about 14.0× faster**  
  (`15900.4472 / 1137.3868 ≈ 13.98`)

---

## Hardware Counter Analysis (Perf, Cascade Lake, 30 GB Logs, 1 Thread, Affinity, G++)

To better understand why the branchless tokenizer performs better, additional experiments were run using Linux `perf` on a 30 GB logs dataset.

### Total Instructions Executed

| Method | With Tokenization | Without Tokenization | Diff |
|-------|-------------------:|---------------------:|---------------------:|
| REGEX String/Vector<string> | 6,911,666,206,378 | 73,619,754,725 | 6,838,046,451,653 |
| STRTOK CharPointer/Vector<CharPointer> | 5,451,103,044,669 | 74,858,728,299 | 5,376,244,316,370 |
| Branchless | 285,925,440,099 | 74,856,963,731 | 211,068,476,368 |

### Branch Misses

| Method | With Tokenization | Without Tokenization | Diff |
|-------|-------------------:|---------------------:|-------------------:|
| REGEX String/Vector<string> | 12,480,936,243 | 23,415,125 | 12,457,521,118 |
| STRTOK CharPointer/Vector<CharPointer> | 3,733,609,961 | 22,735,041 | 3,710,874,920 |
| Branchless | 572,302,959 | 23,060,400 | 549,242,559 |

### Cache Misses

| Method | With Tokenization | Without Tokenization | Diff |
|-------|-------------------:|---------------------:|------------------:|
| REGEX String/Vector<string> | 638,245,765 | 318,944,719 | 319,301,046 |
| STRTOK CharPointer/Vector<CharPointer> | 725,804,313 | 372,501,290 | 353,303,023 |
| Branchless | 654,684,764 | 375,442,076 | 279,242,688 |

---

## Interpretation

These low-level measurements help explain the observed throughput gains:

- The **branchless tokenizer executes far fewer instructions** than REGEX and STRTOK.
- It also produces **far fewer branch misses**, which is consistent with its branch-reduced design.
- Cache misses remain significant for all methods due to the large-scale memory-intensive nature of the workload, but the branchless version still shows the **lowest cache-miss difference** among the three methods in these experiments.

The most important result is the reduction in instruction count:

- REGEX tokenization overhead: **6.838 trillion** extra instructions
- STRTOK tokenization overhead: **5.376 trillion** extra instructions
- Branchless tokenization overhead: **211.1 billion** extra instructions

This large reduction in executed instructions is a major reason the branchless tokenizer achieves much higher throughput.

---

## Summary

Overall, the branchless tokenizer demonstrates:

- substantially higher throughput than REGEX and STRTOK
- strong scaling across thread counts
- additional gains from NUMA-aware thread affinity
- much lower instruction overhead
- fewer branch misses during tokenization

These results show that branchless character classification can be highly effective for large-scale text processing workloads on multicore NUMA systems.