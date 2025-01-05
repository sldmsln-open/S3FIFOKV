#include "s3fifo_rocksdb.hpp"
#include <iostream>
#include <cassert>

void runPaperExample() {
    std::cout << "\n=== Running Paper Example Test ===\n";
    
    // Create S3-FIFO cache with size for 10 objects
    // Assuming each object is ~1MB for this test
    const size_t OBJECT_SIZE = 1 * 1024 * 1024;  // 1MB
    S3FIFORocksDB cache("/mnt/nvme0n1/s3fifokv/data/1",
                        10 * OBJECT_SIZE,  // Total size
                        0.1,              // Small queue 10%
                        0.1);             // Ghost queue 10%

    std::cout << "\nInitial state:\n";
    cache.printState();

    // Test sequence from paper
    std::cout << "\nAccessing objects in sequence...\n";
    
    // First access sequence
    cache.put("A", "valueA");
    cache.put("B", "valueB");
    cache.put("C", "valueC");
    
    std::cout << "\nAfter first three insertions (A,B,C):\n";
    cache.printState();

    // Access A again - should trigger promotion
    std::string value;
    cache.get("A", &value);
    std::cout << "\nAfter accessing A again:\n";
    cache.printState();

    // Continue with more insertions
    cache.put("D", "valueD");
    cache.put("E", "valueE");
    cache.put("F", "valueF");
    cache.put("G", "valueG");
    cache.put("H", "valueH");
    cache.put("I", "valueI");
    cache.put("J", "valueJ");
    
    std::cout << "\nAfter inserting D through J:\n";
    cache.printState();

    // Final insertion should trigger eviction
    cache.put("K", "valueK");
    
    std::cout << "\nFinal state after inserting K:\n";
    cache.printState();

    // Verify expected state
    // 1. K should be in small queue
    bool k_in_small = cache.get("K", &value).ok();
    std::cout << "\nVerification:\n";
    std::cout << "K in small queue: " << (k_in_small ? "Yes" : "No") << "\n";

    // 2. A should be in main queue with count 2
    bool a_in_main = cache.get("A", &value).ok();
    std::cout << "A in cache with multiple accesses: " << (a_in_main ? "Yes" : "No") << "\n";

    // 3. J should be in ghost queue
    // We can indirectly test this by checking if J was evicted
    bool j_evicted = !cache.get("J", &value).ok();
    std::cout << "J evicted (should be in ghost): " << (j_evicted ? "Yes" : "No") << "\n";
}

void runScanResistanceTest() {
    std::cout << "\n=== Running Scan Resistance Test ===\n";
    
    const size_t OBJECT_SIZE = 1 * 1024 * 1024;  // 1MB
    S3FIFORocksDB cache("/tmp/s3fifo_scan_test",
                        10 * OBJECT_SIZE,
                        0.1,
                        0.1);

    // First, establish some hot items
    for (int i = 0; i < 3; i++) {
        for (char c = 'A'; c <= 'C'; c++) {
            std::string key(1, c);
            cache.put(key, "value" + key);
            std::string value;
            cache.get(key, &value);  // Access to increase count
        }
    }

    std::cout << "\nAfter establishing hot items (A,B,C):\n";
    cache.printState();

    // Now perform a scan operation
    std::cout << "\nPerforming scan operation (X1-X20)...\n";
    for (int i = 1; i <= 20; i++) {
        std::string key = "X" + std::to_string(i);
        cache.put(key, "scan_value");
    }

    std::cout << "\nAfter scan operation:\n";
    cache.printState();

    // Verify hot items survived the scan
    std::string value;
    bool hot_items_survived = true;
    for (char c = 'A'; c <= 'C'; c++) {
        std::string key(1, c);
        if (!cache.get(key, &value).ok()) {
            hot_items_survived = false;
            break;
        }
    }

    std::cout << "\nHot items survived scan: " << (hot_items_survived ? "Yes" : "No") << "\n";
}

int main() {
    // Run paper's example test
    runPaperExample();

    // Run scan resistance test
    runScanResistanceTest();

    return 0;
} 