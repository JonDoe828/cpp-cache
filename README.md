# cpp-cache

A modular, thread-safe cache framework implementing multiple page replacement strategies.

## Implemented Policies

- **LRU (Least Recently Used)**  
  O(1) operations using hash map + doubly-linked list.

- **LFU (Least Frequently Used)**  
  O(1) average complexity via frequency buckets and constant-time promotion.

- **ARC (Adaptive Replacement Cache)**  
  Architecture reserved for adaptive strategy integration.

## Highlights

- Policy abstraction with interchangeable eviction strategies
- Thread-safe design with controlled synchronization scope
- Deterministic eviction behavior
- RAII-compliant memory management
- Clean separation between cache interface and replacement policy