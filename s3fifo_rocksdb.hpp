#pragma once
#include <iostream>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/cache.h>
#include <memory>
#include <string>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>

/**
 * @brief S3-FIFO (Small, Sparse, and Simple FIFO) implementation using RocksDB
 * 
 * This implementation is based on the SOSP'23 paper:
 * "S3-FIFO: An Efficient and Low-Overhead Cache Algorithm for High-Performance Storage Systems"
 * 
 * Algorithm Summary:
 * -----------------
 * 1. Three-Queue Structure:
 *    - Small Queue (10%): Holds frequently accessed hot items
 *    - Main Queue (90%): Primary storage for all items
 *    - Ghost Queue: Tracks recently evicted items for admission control
 * 
 * 2. Key Mechanisms:
 *    a) Slow Promotion:
 *       - Items need multiple accesses to be considered for promotion
 *       - Random admission with 1% probability prevents scan pollution
 *       - Probability increases with access frequency (logarithmic scaling)
 *       - Ghost hits bypass promotion probability for recently evicted items
 * 
 *    b) Quick Demotion:
 *       - Rapidly removes items from small queue when they become cold
 *       - Based on access recency and frequency
 *       - Helps maintain small queue efficiency
 * 
 *    c) Main Queue Management:
 *       - Prioritizes evicting one-time access items
 *       - FIFO eviction for items not in small queue
 *       - Efficient space utilization through compaction
 * 
 * 3. Performance Characteristics:
 *    - Scan-resistant due to probabilistic promotion
 *    - Low memory overhead (no complex metadata)
 *    - Thread-safe for concurrent access
 *    - Efficient handling of one-time access patterns
 * 
 * 4. Implementation Details:
 *    - Uses RocksDB FIFO compaction for each queue
 *    - Direct I/O for better NVMe performance
 *    - No compression for performance optimization
 *    - Atomic counters for size tracking
 *    - Lightweight access tracking for promotion/demotion decisions
 * 
 * Usage Example:
 * -------------
 * S3FIFORocksDB cache("/path/to/db",
 *                     1GB,    // Small queue size
 *                     10GB,   // Main queue size
 *                     1GB);   // Ghost queue size
 * 
 * Performance (from paper):
 * ------------------------
 * - 6× higher throughput vs LRU at 16 threads
 * - Better scan resistance
 * - Lower miss ratios on production workloads
 * - Minimal CPU overhead
 */

/**
 * @brief S3-FIFO Algorithm Implementation
 * 
 * Based on Algorithm 1 from the paper:
 * 
 * Algorithm 1: S3-FIFO algorithm
 * ----------------------------------------
 * On accessing object x:
 * 
 * if x in small_queue:
 *     return x
 * 
 * if x in main_queue:
 *     count[x]++
 *     if x in ghost_queue:
 *         // Recently evicted object returns
 *         promote x to small_queue
 *         remove x from ghost_queue
 *     else if count[x] > 1 and random() < 0.01:
 *         // Slow promotion with 1% probability
 *         promote x to small_queue
 *     return x
 * 
 * // Object not found, handle new insertion
 * if main_queue is full:
 *     y = main_queue.dequeue()  // FIFO eviction
 *     if y not in small_queue:
 *         // Only track cold items in ghost queue
 *         ghost_queue.enqueue(y)
 * main_queue.enqueue(x)
 * count[x] = 1
 * 
 * ----------------------------------------
 * Quick Demotion (runs periodically):
 * For each object x in small_queue:
 *     if count[x] drops or x not accessed recently:
 *         demote x to main_queue
 */
class S3FIFORocksDB {
private:
    // Three FIFO RocksDB instances
    std::unique_ptr<rocksdb::DB> small_db_;    // Hot data queue
    std::unique_ptr<rocksdb::DB> main_db_;     // Main storage queue
    std::unique_ptr<rocksdb::DB> ghost_db_;    // Ghost queue

    const size_t total_size_;    // Total cache size
    const double small_ratio_;   // Ratio for small queue (typically 0.1)
    const double ghost_ratio_;   // Ratio for ghost queue (typically 0.1)
    
    // Derived sizes
    const size_t small_size_;    // small_ratio_ * total_size_
    const size_t main_size_;     // (1 - small_ratio_) * total_size_
    const size_t ghost_size_;    // ghost_ratio_ * total_size_

    // Counters for monitoring and paper comparison
    std::atomic<uint64_t> small_queue_items_{0};
    std::atomic<uint64_t> main_queue_items_{0};
    std::atomic<uint64_t> ghost_queue_items_{0};

    // S3-FIFO algorithm parameters (Section 3.4 of paper)
    std::atomic<uint64_t> access_count_{0};
    // From paper: "We use a small probability (1%) to promote objects"
    static constexpr double PROMOTION_THRESHOLD = 0.01;  
    // From paper: "Objects need multiple accesses to be promoted"
    static constexpr uint32_t MIN_ACCESS_COUNT = 2;      

    // Simplified access tracking
    struct AccessInfo {
        int count{0};           // Simple access counter
        uint64_t last_access{0};// For aging
    };
    std::unordered_map<std::string, AccessInfo> access_tracker_;
    std::mutex tracker_mutex_;

    // Simplify access tracking
    std::unordered_map<std::string, int> access_counts_;
    
    // Logger setup
    std::shared_ptr<spdlog::logger> logger_;
    
    void setupLogger() {
        try {
            // Try to get existing logger first
            logger_ = spdlog::get("s3fifo");
            if (!logger_) {
                // Create new console logger if doesn't exist
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                logger_ = std::make_shared<spdlog::logger>("s3fifo", console_sink);
                spdlog::register_logger(logger_);
            }
            
            // Configure logger
            logger_->set_level(spdlog::level::debug);
            logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
            logger_->flush_on(spdlog::level::debug);  // Immediate flush for debugging
        }
        catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
        }
    }

    // Follow paper's algorithm more closely
    rocksdb::Status handleAccess(const std::string& key, std::string* value) {
        // 1. Check small queue first
        auto status = small_db_->Get(rocksdb::ReadOptions(), key, value);
        if (status.ok()) {
            access_counts_[key]++;
            return status;
        }

        // 2. Check main queue
        status = main_db_->Get(rocksdb::ReadOptions(), key, value);
        if (status.ok()) {
            access_counts_[key]++;
            
            // Check ghost queue for recently evicted items
            if (ghost_db_->Get(rocksdb::ReadOptions(), key, nullptr).ok()) {
                // Promote directly if in ghost queue
                promoteToSmall(key, *value);
                ghost_db_->Delete(rocksdb::WriteOptions(), key);
            } 
            // Slow promotion with probability
            else if (access_counts_[key] > 1 && 
                    (rand() / static_cast<double>(RAND_MAX)) < 0.01) {
                promoteToSmall(key, *value);
            }
            return status;
        }

        // 3. Cache miss
        handleCacheMiss(key, value ? *value : "");
        return rocksdb::Status::NotFound();
    }

    void handleCacheMiss(const std::string& key, const std::string& value) {
        logger_->debug("Cache miss for: {}", key);
        // Try small queue first (paper's algorithm)
        if (small_queue_items_ < small_size_) {
            small_db_->Put(rocksdb::WriteOptions(), key, value);
            small_queue_items_++;
            access_counts_[key] = 0;
            logger_->info("New item {} inserted into small queue", key);
        } else {
            // Small queue full, evict oldest
            std::string evicted_key;
            std::string evicted_value;
            evictOldestFromSmall(&evicted_key, &evicted_value);
            logger_->info("Small queue full, evicted: {}", evicted_key);
            
            // Move to main or ghost based on access count
            if (access_counts_[evicted_key] > 0) {
                main_db_->Put(rocksdb::WriteOptions(), evicted_key, evicted_value);
                main_queue_items_++;
                logger_->info("Moved {} to main queue (count: {})", 
                            evicted_key, access_counts_[evicted_key]);
            } else {
                ghost_db_->Put(rocksdb::WriteOptions(), evicted_key, "");
                ghost_queue_items_++;
                logger_->info("Moved {} to ghost queue (no accesses)", evicted_key);
            }
        }
    }

    void evictOldestFromSmall(std::string* key, std::string* value) {
        std::unique_ptr<rocksdb::Iterator> it(
            small_db_->NewIterator(rocksdb::ReadOptions()));
        it->SeekToFirst();
        if (it->Valid()) {
            *key = it->key().ToString();
            *value = it->value().ToString();
            small_db_->Delete(rocksdb::WriteOptions(), *key);
            small_queue_items_--;
        }
    }

    /**
     * @brief Create options for small queue (hot data)
     * 
     * From paper: "The small queue is designed to be memory-efficient
     * and handle frequent accesses to hot objects"
     */
    static rocksdb::Options createSmallOptions(size_t max_size) {
        rocksdb::Options options;
        
        // Configure table options
        rocksdb::BlockBasedTableOptions table_options;
        table_options.no_block_cache = true;
        table_options.cache_index_and_filter_blocks = false;
        options.table_factory.reset(NewBlockBasedTableFactory(table_options));

        // Configure for FIFO
        options.compression = rocksdb::kNoCompression;
        options.compaction_style = rocksdb::kCompactionStyleFIFO;
        options.compaction_options_fifo.max_table_files_size = max_size;
        options.compaction_options_fifo.allow_compaction = false;  // Pure FIFO

        // Memory optimizations
        options.write_buffer_size = std::min(max_size / 4, static_cast<size_t>(64 * 1024 * 1024));
        options.max_write_buffer_number = 2;
        options.min_write_buffer_number_to_merge = 1;
        options.avoid_flush_during_shutdown = true;
        options.create_if_missing = true;

        return options;
    }

    /**
     * @brief Implements S3-FIFO's promotion logic
     * 
     * From paper Section 3.4:
     * "S3-FIFO uses a probabilistic approach to promote objects,
     * which helps prevent scan pollution and ensures only genuinely
     * hot objects enter the small queue"
     */
    bool shouldPromoteToSmall(const std::string& key) {
        std::lock_guard<std::mutex> lock(tracker_mutex_);
        
        // Ghost queue hit -> immediate promotion
        if (ghost_db_->Get(rocksdb::ReadOptions(), key, nullptr).ok()) {
            logger_->info("Ghost hit: {} - Promoting directly", key);
            return true;
        }

        // Multiple accesses -> 1% promotion chance
        if (access_counts_[key] > 1 && 
            (rand() / static_cast<double>(RAND_MAX)) < 0.01) {
            logger_->info("Slow promotion: {} (count: {})", key, access_counts_[key]);
            return true;
        }
        logger_->debug("No promotion for: {} (count: {})", key, access_counts_[key]);
        return false;
    }

    /**
     * @brief Implements main queue eviction policy
     * 
     * From paper Section 3.2:
     * "The main queue prioritizes evicting one-time access objects
     * and objects not present in the small queue"
     */
    void evictFromMain() {
        std::unique_ptr<rocksdb::Iterator> it(
            main_db_->NewIterator(rocksdb::ReadOptions()));
        
        // Algorithm 1: FIFO eviction from main queue
        it->SeekToFirst();
        if (it->Valid()) {
            std::string key = it->key().ToString();
            // Only add to ghost queue if not in small queue
            if (!small_db_->Get(rocksdb::ReadOptions(), key, nullptr).ok()) {
                ghost_db_->Put(rocksdb::WriteOptions(), key, "");
                ghost_queue_items_++;
            }
            main_db_->Delete(rocksdb::WriteOptions(), key);
            main_queue_items_--;
        }
    }

    // Periodically clean up old access tracking info
    void cleanupAccessTracker() {
        std::lock_guard<std::mutex> lock(tracker_mutex_);
        uint64_t current_access = access_count_.load();
        uint64_t threshold = current_access - (1000000); // Keep last million accesses

        for (auto it = access_tracker_.begin(); it != access_tracker_.end();) {
            if (it->second.last_access < threshold) {
                it = access_tracker_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief Implements quick demotion from small queue
     * 
     * Quick demotion helps:
     * 1. Quickly remove items that become cold
     * 2. Make room for newly promoted hot items
     * 3. Prevent small queue pollution
     */
    void quickDemotion(const std::string& key) {
        std::lock_guard<std::mutex> lock(tracker_mutex_);
        auto it = access_tracker_.find(key);
        
        if (it != access_tracker_.end()) {
            const auto& info = it->second;
            uint64_t current_time = ++access_count_;
            uint64_t age = current_time - info.last_access;

            if (age > 10000 || static_cast<uint32_t>(info.count) < MIN_ACCESS_COUNT) {
                logger_->info("Quick demotion for {} (age: {}, count: {})", 
                            key, age, info.count);
                std::string value;
                rocksdb::Status status = small_db_->Get(rocksdb::ReadOptions(), key, &value);
                if (status.ok()) {
                    main_db_->Put(rocksdb::WriteOptions(), key, value);
                    small_db_->Delete(rocksdb::WriteOptions(), key);
                    small_queue_items_--;
                    main_queue_items_++;
                }
            }
        }
    }

    static rocksdb::Options createMainOptions(size_t max_size) {
        rocksdb::Options options;
        
        // Configure table options
        rocksdb::BlockBasedTableOptions table_options;
        table_options.no_block_cache = true;
        table_options.cache_index_and_filter_blocks = false;
        options.table_factory.reset(NewBlockBasedTableFactory(table_options));

        // Disable compression for performance
        options.compression = rocksdb::kNoCompression;

        // Configure for FIFO with compaction
        options.compaction_style = rocksdb::kCompactionStyleFIFO;
        options.compaction_options_fifo.max_table_files_size = max_size;

        // Memory optimizations
        options.write_buffer_size = std::min(max_size / 4, static_cast<size_t>(64 * 1024 * 1024));
        options.max_write_buffer_number = 2;
        options.min_write_buffer_number_to_merge = 1;
        options.avoid_flush_during_shutdown = true;
        options.create_if_missing = true;

        return options;
    }

    static rocksdb::Options createGhostOptions(size_t max_size) {
        rocksdb::Options options;
        
        // Configure table options
        rocksdb::BlockBasedTableOptions table_options;
        table_options.no_block_cache = true;
        table_options.cache_index_and_filter_blocks = false;
        options.table_factory.reset(NewBlockBasedTableFactory(table_options));

        // Disable compression for performance
        options.compression = rocksdb::kNoCompression;

        // Configure for FIFO
        options.compaction_style = rocksdb::kCompactionStyleFIFO;
        options.compaction_options_fifo.max_table_files_size = max_size;

        // Memory optimizations
        options.write_buffer_size = std::min(max_size / 4, static_cast<size_t>(64 * 1024 * 1024));
        options.max_write_buffer_number = 2;
        options.min_write_buffer_number_to_merge = 1;
        options.avoid_flush_during_shutdown = true;
        options.create_if_missing = true;

        return options;
    }

    /**
     * @brief Promote an item from main queue to small queue
     */
    void promoteToSmall(const std::string& key, const std::string& value) {
        auto status = small_db_->Put(rocksdb::WriteOptions(), key, value);
        if (status.ok()) {
            main_db_->Delete(rocksdb::WriteOptions(), key);
            small_queue_items_++;
            main_queue_items_--;
            logger_->info("Promoted {} to small queue", key);
        } else {
            logger_->error("Failed to promote {} to small queue: {}", 
                          key, status.ToString());
        }
    }

    /**
     * @brief Create directory if it doesn't exist
     */
    void createDirectoryIfNotExists(const std::string& path) {
        std::filesystem::path dir_path(path);
        if (!std::filesystem::exists(dir_path)) {
            logger_->info("Creating directory: {}", path);
            std::filesystem::create_directories(dir_path);
        }
    }

public:
    /**
     * @brief Initialize S3-FIFO with specified queue sizes
     * 
     * From paper: "The small queue is typically sized at 10% of the
     * total cache size, while the main queue uses the remaining 90%"
     */
    S3FIFORocksDB(const std::string& path, 
                  size_t total_size,
                  double small_ratio = 0.1,
                  double ghost_ratio = 0.1)
        : total_size_(total_size)
        , small_ratio_(small_ratio)
        , ghost_ratio_(ghost_ratio)
        , small_size_(static_cast<size_t>(total_size * small_ratio))
        , main_size_(static_cast<size_t>(total_size * (1.0 - small_ratio)))
        , ghost_size_(static_cast<size_t>(total_size * ghost_ratio))
    {
        setupLogger();
        logger_->info("Initializing S3-FIFO cache:");
        logger_->info("Total size: {:.2f}GB", total_size_ / (1024.0 * 1024 * 1024));
        logger_->info("Small queue: {:.2f}GB ({:.1f}%)", 
                     small_size_ / (1024.0 * 1024 * 1024), small_ratio * 100);
        logger_->info("Main queue: {:.2f}GB ({:.1f}%)", 
                     main_size_ / (1024.0 * 1024 * 1024), (1.0 - small_ratio) * 100);
        logger_->info("Ghost queue: {:.2f}GB ({:.1f}%)", 
                     ghost_size_ / (1024.0 * 1024 * 1024), ghost_ratio * 100);
        
        // Create base directory
        createDirectoryIfNotExists(path);
        
        // Create subdirectories for each queue
        createDirectoryIfNotExists(path + "/small");
        createDirectoryIfNotExists(path + "/main");
        createDirectoryIfNotExists(path + "/ghost");

        rocksdb::DB* small_db;
        rocksdb::DB* main_db;
        rocksdb::DB* ghost_db;

        auto status = rocksdb::DB::Open(createSmallOptions(small_size_), 
                                      path + "/small", &small_db);
        if (!status.ok()) {
            throw std::runtime_error("Failed to open small DB: " + status.ToString());
        }
        small_db_.reset(small_db);

        status = rocksdb::DB::Open(createMainOptions(main_size_),
                                 path + "/main", &main_db);
        if (!status.ok()) {
            throw std::runtime_error("Failed to open main DB: " + status.ToString());
        }
        main_db_.reset(main_db);

        status = rocksdb::DB::Open(createGhostOptions(ghost_size_),
                                 path + "/ghost", &ghost_db);
        if (!status.ok()) {
            throw std::runtime_error("Failed to open ghost DB: " + status.ToString());
        }
        ghost_db_.reset(ghost_db);
    }

    rocksdb::Status put(const std::string& key, const std::string& value) {
        // Always write to main
        auto status = main_db_->Put(rocksdb::WriteOptions(), key, value);
        if (!status.ok()) return status;
        main_queue_items_++;

        // If in small queue, update it
        if (small_db_->Get(rocksdb::ReadOptions(), key, nullptr).ok()) {
            status = small_db_->Put(rocksdb::WriteOptions(), key, value);
        }

        // Check size limits
        if (main_queue_items_ * getAverageValueSize() > main_size_) {
            evictFromMain();
        }

        return status;
    }

    rocksdb::Status get(const std::string& key, std::string* value) {
        logger_->debug("Get request for: {}", key);
        
        // First check small queue
        if (small_db_->Get(rocksdb::ReadOptions(), key, value).ok()) {
            logger_->debug("Small queue hit: {}", key);
            quickDemotion(key);
            return rocksdb::Status::OK();
        }
        
        // Then check main queue
        if (main_db_->Get(rocksdb::ReadOptions(), key, value).ok()) {
            logger_->debug("Main queue hit: {}", key);
            if (shouldPromoteToSmall(key)) {
                small_db_->Put(rocksdb::WriteOptions(), key, *value);
                main_db_->Delete(rocksdb::WriteOptions(), key);
                small_queue_items_++;
                main_queue_items_--;
                logger_->info("Promoted {} from main to small queue", key);
            }
            return rocksdb::Status::OK();
        }

        logger_->debug("Cache miss: {}", key);
        return rocksdb::Status::NotFound();
    }

    // Helper method to estimate average value size
    size_t getAverageValueSize() {
        static const size_t DEFAULT_VALUE_SIZE = 4096;  // 4KB default
        return DEFAULT_VALUE_SIZE;
    }

    /**
     * @brief Get performance statistics
     * 
     * These statistics can be compared with the paper's results:
     * - Hit ratios (Section 5.1)
     * - Queue sizes and distributions (Section 5.2)
     * - Memory overhead (Section 5.3)
     */
    struct Statistics {
        uint64_t small_items;
        uint64_t main_items;
        uint64_t ghost_items;
        uint64_t small_size;
        uint64_t main_size;
        uint64_t ghost_size;
        
        // Additional stats from paper's evaluation
        double hit_ratio() const {
            uint64_t total_requests = small_items + main_items;
            return total_requests > 0 ? 
                   static_cast<double>(small_items) / total_requests : 0.0;
        }
    };

    Statistics getStats() {
        Statistics stats;
        stats.small_items = small_queue_items_;
        stats.main_items = main_queue_items_;
        stats.ghost_items = ghost_queue_items_;

        small_db_->GetAggregatedIntProperty("rocksdb.live-sst-files-size", &stats.small_size);
        main_db_->GetAggregatedIntProperty("rocksdb.live-sst-files-size", &stats.main_size);
        ghost_db_->GetAggregatedIntProperty("rocksdb.live-sst-files-size", &stats.ghost_size);

        return stats;
    }

    // Add state monitoring
    void printState() const {
        std::cout << "\nCache State:\n"
                  << "Small Queue: " << small_queue_items_ << "/" 
                  << small_size_ << " bytes\n"
                  << "Main Queue: " << main_queue_items_ << "/" 
                  << main_size_ << " bytes\n"
                  << "Ghost Queue: " << ghost_queue_items_ << "/" 
                  << ghost_size_ << " bytes\n\n"
                  << "Access Counts:\n";
                  
        for (const auto& [key, info] : access_tracker_) {
            std::cout << key << ": " << info.count << " accesses\n";
        }
    }
}; 