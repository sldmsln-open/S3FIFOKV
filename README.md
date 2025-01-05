# S3-FIFO RocksDB Implementation

This is an implementation of the S3-FIFO (Small, Sparse, and Simple FIFO) cache algorithm using RocksDB, based on the SOSP'23 paper.

## Algorithm Overview

### Core Algorithm (From Paper) 

### Queue Structure

## Implementation Details

### 1. Queue Management
- Small Queue: RocksDB instance with in-memory optimization
- Main Queue: RocksDB instance with NVMe optimization
- Ghost Queue: RocksDB instance for tracking evicted items

### RocksDB Configuration Details

#### Small Queue (In-Memory Hot Data)
```cpp
// Small queue options
rocksdb::Options small_opts;

// Disable block cache - we are the cache
rocksdb::BlockBasedTableOptions table_options;
table_options.no_block_cache = true;
table_options.cache_index_and_filter_blocks = false;

// Pure FIFO without compaction
small_opts.compaction_style = rocksdb::kCompactionStyleFIFO;
small_opts.compaction_options_fifo.max_table_files_size = small_size;
small_opts.compaction_options_fifo.allow_compaction = false;  // No compaction

// Memory optimizations
small_opts.write_buffer_size = 64 * 1024 * 1024;  // 64MB
small_opts.max_write_buffer_number = 2;
small_opts.min_write_buffer_number_to_merge = 1;
small_opts.avoid_flush_during_shutdown = true;
```

Purpose:
- Pure in-memory storage for hot data
- No compaction to minimize overhead
- Small write buffers for memory efficiency
- No block cache as we are the cache

#### Main Queue (NVMe Storage)
```cpp
// Main queue options
rocksdb::Options main_opts;

// Disable block cache - we are the cache
table_options.no_block_cache = true;
table_options.cache_index_and_filter_blocks = false;

// FIFO with compaction
main_opts.compaction_style = rocksdb::kCompactionStyleFIFO;
main_opts.compaction_options_fifo.max_table_files_size = main_size;
main_opts.compaction_options_fifo.allow_compaction = true;

// NVMe optimizations
main_opts.write_buffer_size = 256 * 1024 * 1024;  // 256MB
main_opts.max_write_buffer_number = 4;
main_opts.target_file_size_base = 256 * 1024 * 1024;  // 256MB

// Direct I/O for better NVMe performance
main_opts.use_direct_io_for_flush_and_compaction = true;
main_opts.use_direct_reads = true;
```

Purpose:
- FIFO compaction for automatic size management
- Larger write buffers for NVMe performance
- Direct I/O for better NVMe throughput
- No block cache as we implement caching logic

#### Ghost Queue (Tracking Evicted Items)
```cpp
// Ghost queue options
rocksdb::Options ghost_opts;

// Disable block cache - we are the cache
table_options.no_block_cache = true;
table_options.cache_index_and_filter_blocks = false;

// Simple FIFO with compaction
ghost_opts.compaction_style = rocksdb::kCompactionStyleFIFO;
ghost_opts.compaction_options_fifo.max_table_files_size = ghost_size;
ghost_opts.compaction_options_fifo.allow_compaction = true;

// Basic settings
ghost_opts.write_buffer_size = 64 * 1024 * 1024;  // 64MB
ghost_opts.max_write_buffer_number = 2;
```

Purpose:
- Track recently evicted items
- FIFO compaction for automatic cleanup
- Minimal resource usage
- No block cache needed

### Key Design Points
1. No RocksDB Block Cache
   - We disable RocksDB's internal block cache completely
   - Our implementation provides the caching logic
   - Avoids double-caching and memory waste

2. FIFO Management
   - Small Queue: Pure FIFO, no compaction
   - Main Queue: FIFO with compaction for size control
   - Ghost Queue: FIFO with compaction for tracking

3. Memory vs NVMe Optimization
   - Small Queue: Optimized for memory performance
   - Main Queue: Optimized for NVMe throughput
   - Ghost Queue: Minimal resource usage

4. Performance Optimizations
   - No compression in any queue for better performance
   - Direct I/O for NVMe operations
   - Optimized write buffer sizes for each queue type

### 2. Key Mechanisms

#### Slow Promotion

## Test Cases

### 1. Paper Example Test
Tests the basic functionality with sequence:

## References

1. Original Paper: "S3-FIFO: An Efficient and Low-Overhead Cache Algorithm for High-Performance Storage Systems" (SOSP'23)
2. RocksDB Documentation: https://github.com/facebook/rocksdb/wiki

other details:
┌──────────────┐
│ Small Queue │ (10% of total size)
│ Hot Items │
└──────────────┘
↕
┌──────────────┐
│ Main Queue │ (90% of total size)
│ All Items │
└──────────────┘
↕
┌──────────────┐
│ Ghost Queue │ (10% of total size)
│ Evicted Items│
└──────────────┘

Algorithm 1: S3-FIFO algorithm
----------------------------------------
On accessing object x:
if x in small_queue:
    return x
if x in main_queue:
    count[x]++
if x in ghost_queue:
    // Recently evicted object returns
    promote x to small_queue
    remove x from ghost_queue
else if count[x] > 1 and random() < 0.01:
    // Slow promotion with 1% probability
promote x to small_queue
    return x
// Object not found, handle new insertion
if main_queue is full:
    y = main_queue.dequeue() // FIFO eviction
if y not in small_queue:
    // Only track cold items in ghost queue
    ghost_queue.enqueue(y)
    main_queue.enqueue(x)
    count[x] = 1

## Building and Running

### Install Dependencies

For Ubuntu/Debian:
```bash
sudo apt-get install libgflags-dev libspdlog-dev libsnappy-dev libbz2-dev liblz4-dev libzstd-dev

# Install RocksDB from source for latest version
git clone https://github.com/facebook/rocksdb.git
cd rocksdb
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j$(nproc)
sudo make install
```

For CentOS/RHEL:
```bash
sudo yum install gflags-devel spdlog-devel snappy-devel rocksdb-devel bzip2-devel lz4-devel libzstd-devel
```

For Fedora:
```bash
sudo dnf install gflags-devel spdlog-devel snappy-devel rocksdb-devel bzip2-devel lz4-devel libzstd-devel
```

### Alternative: Build Dependencies from Source

If packages are not available, build from source:

```bash
# Install Snappy
git clone https://github.com/google/snappy.git
cd snappy
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install

# Install gflags
git clone https://github.com/gflags/gflags.git
cd gflags
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install

# Install spdlog
git clone https://github.com/gabime/spdlog.git
cd spdlog
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install

# Install lz4
git clone https://github.com/lz4/lz4.git
cd lz4
make
sudo make install

# Install zstd
git clone https://github.com/facebook/zstd.git
cd zstd
make -j$(nproc)
sudo make install
```

### Build S3-FIFO

```bash
# Update shared library path
sudo ldconfig

# Build project
mkdir build && cd build
cmake ..
make

# Run tests
./s3fifo_rocksdb
```

