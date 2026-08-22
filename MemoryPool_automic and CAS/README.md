# MemoryPool — Atomic & CAS Based Memory Pool in C++

A lightweight fixed-size memory pool implemented in modern C++.

This project was built to explore low-level memory management, memory reuse, pointer alignment, free lists, atomic operations, and Compare-And-Swap (CAS). It also benchmarks the custom memory pool against the standard system allocator under different allocation patterns.

The current implementation combines:

- Block-based memory allocation
- Fixed-size slots
- Memory reuse through a free list
- `std::atomic`
- Compare-And-Swap (CAS)
- RAII locking for block allocation
- Placement new for object construction
- Multiple size classes through `HashBucket`

---

## 1. Project Structure

```text
MemoryPool/
│
├── include/
│   └── MemoryPool.h
│
├── src/
│   ├── MemoryPool.cpp
│   └── main.cpp
│
└── README.md
```

`MemoryPool.h` contains the main data structures, class declarations, `HashBucket`, and template helpers.

`MemoryPool.cpp` contains the implementation of block allocation, slot allocation/deallocation, free-list operations, and CAS logic.

`main.cpp` is currently used for benchmarking the custom allocator against the system allocator.

---

## 2. Design Overview

Instead of requesting memory from the operating system for every small allocation, `MemoryPool` requests a larger block of memory and divides it into fixed-size slots.

Conceptually:

```text
System Heap
    |
    | operator new(BlockSize)
    v
+---------------------------------------------------+
| Block Header | Padding | Slot | Slot | Slot | ... |
+---------------------------------------------------+
                         ^
                         |
                      curSlot_
```

For example, a pool configured with:

```cpp
MemoryPool pool;
pool.init(32);
```

manages 32-byte slots.

The pool allocates a large block once and then serves individual allocation requests from that block.

---

## 3. Main Data Structures

### Slot

Each slot can have two different roles depending on whether it is currently allocated.

When allocated:

```text
+----------------------+
| User Data            |
|                      |
+----------------------+
```

When released:

```text
+----------------------+
| next pointer         | ---> next free slot
| unused memory        |
+----------------------+
```

The beginning of the released slot is reused as a free-list node.

The implementation uses:

```cpp
struct Slot
{
    std::atomic<Slot*> next;
};
```

This allows the free list to be manipulated using atomic operations and CAS.

---

## 4. Block Allocation

When the current block runs out of unused slots, the pool requests another block:

```cpp
operator new(BlockSize_);
```

Blocks themselves form a linked list:

```text
firstBlock_
    |
    v
 Block C
    |
    v
 Block B
    |
    v
 Block A
    |
  nullptr
```

The block list is required so that all allocated blocks can eventually be released by the `MemoryPool` destructor.

The memory pool therefore distinguishes between two levels of memory management:

```text
Block
  |
  +-- Slot
  +-- Slot
  +-- Slot
  +-- Slot
```

Slots are reused during normal operation.

Blocks are returned to the system when the entire `MemoryPool` is destroyed.

---

## 5. Pointer Alignment

The pool calculates padding before the first slot in a block:

```cpp
size_t MemoryPool::padPointer(char* p, size_t align)
{
    size_t rem =
        reinterpret_cast<size_t>(p) % align;

    return rem == 0
        ? 0
        : align - rem;
}
```

The purpose is to move the starting address forward until it satisfies the requested alignment.

Conceptually:

```text
Block Header
     |
     v
+----------+---------+----------------+
| Header   | Padding | First Slot     |
+----------+---------+----------------+
                    ^
                    |
              aligned address
```

`char*` is used for byte-level pointer arithmetic because incrementing a `char*` advances the address by exactly one byte.

---

## 6. Allocation Strategy

`MemoryPool::allocate()` follows two main paths.

### Fast reuse path

The allocator first checks the free list:

```text
allocate()
    |
    v
popFreeList()
    |
    +---- slot available ----> return reused slot
    |
    +---- empty
             |
             v
         curSlot_
```

Previously released memory is therefore preferred over unused memory.

### New-slot path

If the free list is empty, the allocator takes the next unused slot from the current block.

```text
curSlot_
    |
    v
[Slot A][Slot B][Slot C][Slot D]
    |
 return A

curSlot_ moves forward
             |
             v
        [Slot B]
```

If the block is exhausted, `allocateNewBlock()` creates another block.

---

## 7. Deallocation

`deallocate()` does not immediately return memory to the operating system.

Instead:

```cpp
void MemoryPool::deallocate(void* ptr)
{
    if (ptr == nullptr)
        return;

    Slot* slot =
        reinterpret_cast<Slot*>(ptr);

    pushFreeList(slot);
}
```

The released memory is inserted into the free list and can be reused by future allocations.

```text
Allocated Slot
      |
      | deallocate()
      v
   freeList
      |
      v
      A -> B -> C
```

This is the central idea behind the memory pool: memory is recycled rather than repeatedly requested from and returned to the system allocator.

---

## 8. Lock-Free Free List

The free list is implemented using:

```cpp
std::atomic<Slot*> freeList_;
```

and:

```cpp
compare_exchange_weak()
```

The structure is similar to a Treiber stack.

### Push

Before:

```text
freeList_
    |
    v
    B -> C
```

Push `A`:

```text
A -> B -> C
```

After successful CAS:

```text
freeList_
    |
    v
    A -> B -> C
```

Conceptually, CAS performs:

```cpp
if (freeList_ == oldHead)
{
    freeList_ = newHead;
}
else
{
    retry;
}
```

The actual update is atomic, so another thread cannot observe a partially updated head pointer.

---

## 9. Pop

The reverse operation removes the current head:

```text
Before:

freeList_
    |
    v
    A -> B -> C


After:

freeList_
    |
    v
    B -> C

return A
```

The implementation again uses `compare_exchange_weak()`.

If another thread changes the head before the CAS succeeds, the operation retries using the updated head.

---

## 10. Memory Ordering

The free list currently uses explicit memory ordering, including:

```cpp
std::memory_order_relaxed
std::memory_order_acquire
std::memory_order_release
```

The basic intention is:

```text
pushFreeList()
     |
     +-- prepare slot->next
     |
     +-- release new head


popFreeList()
     |
     +-- acquire head
     |
     +-- consume slot
```

`relaxed` ordering is used where only atomicity of the pointer operation is required and no additional synchronization relationship is needed.

This avoids unnecessarily requesting sequential consistency for every atomic operation.

---

## 11. RAII and Block Allocation

The free-list path uses atomic operations and CAS, but block creation and `curSlot_` advancement are protected by a mutex:

```cpp
std::lock_guard<std::mutex> lock(MutexForBlock_);
```

This uses RAII:

```text
lock_guard constructed
        |
        v
    mutex.lock()

       ...

scope ends
        |
        v
   mutex.unlock()
```

Therefore, the current allocator should not be described as completely lock-free.

More precisely:

> The free-list reuse path uses lock-free CAS operations, while block allocation and sequential slot allocation are protected by a mutex.

---

## 12. HashBucket Size Classes

`HashBucket` provides multiple `MemoryPool` instances for different allocation sizes.

The current configuration uses:

```cpp
#define MEMORY_POOL_NUM 64
#define SLOT_BASE_SIZE 8
```

which produces size classes such as:

```text
Pool 0  ->   8 bytes
Pool 1  ->  16 bytes
Pool 2  ->  24 bytes
Pool 3  ->  32 bytes
...
Pool 63 -> 512 bytes
```

The required pool can be calculated using integer ceiling division:

```cpp
((size + SLOT_BASE_SIZE - 1) / SLOT_BASE_SIZE) - 1
```

For example:

```text
size = 17

(17 + 8 - 1) / 8
= 24 / 8
= 3
```

17 bytes therefore require the third size class, which corresponds to a 24-byte slot.

Because array indices start at zero:

```text
size class 3
    |
    v
index 2
```

Hence the final `-1`.

---

## 13. Object Construction

Raw memory allocation and C++ object construction are separated.

The helper:

```cpp
newElement<T>(...)
```

first obtains raw memory from `HashBucket` and then constructs the object using placement new:

```cpp
new(p) T(std::forward<Args>(args)...);
```

Conceptually:

```text
MemoryPool
    |
    v
raw memory
    |
    v
placement new
    |
    v
constructed T object
```

`deleteElement<T>()` performs the reverse operation:

```text
T object
   |
   v
explicit destructor
   |
   v
HashBucket::freeMemory()
   |
   v
MemoryPool::deallocate()
   |
   v
freeList
```

This is necessary because the memory is owned by the pool rather than by a normal `new` expression.

---

# Benchmark

The custom allocator was compared with:

```cpp
::operator new(size)
::operator delete(ptr)
```

rather than directly with `new T` / `delete T`.

This keeps the benchmark focused primarily on raw memory allocation rather than object construction.

Tests were performed using 32-byte allocations.

---

## Benchmark 1 — Batch Allocation

Workload:

```text
allocate
allocate
allocate
...
1,000,000 allocations

then:

deallocate
deallocate
deallocate
...
1,000,000 deallocations
```

The workload was repeated 20 times.

### Results

```text
Memory Allocation Benchmark

Slot size:   32 bytes
Alloc count: 1,000,000
Repeat:      20

MemoryPool:  3073 ms
System new:  3562 ms

MemoryPool speedup: 1.15913x
```

In this workload, the custom memory pool completed the benchmark approximately:

```text
1.16x
```

as fast as the system allocator.

Measured execution time was approximately 13.7% lower:

```text
(3562 - 3073) / 3562 ≈ 13.7%
```

This workload benefits from block-based allocation because many small allocations can be served by sequentially advancing through preallocated blocks.

---

## Benchmark 2 — Immediate Reuse

The second benchmark used a different allocation pattern:

```text
allocate
deallocate
allocate
deallocate
allocate
deallocate
...
```

A total of 20,000,000 iterations were performed.

### Results

```text
Allocate / Deallocate Benchmark

Slot size:       32 bytes
Operation count: 20,000,000

MemoryPool:      3084 ms
System new:      2604 ms

MemoryPool speedup: 0.844358x
Time reduction:     -18.4332%
```

In this workload, the custom allocator was slower than the system allocator.

The system allocator completed the benchmark approximately:

```text
3084 / 2604 ≈ 1.18x
```

as fast as the custom memory pool.

---

## Benchmark Interpretation

The two benchmarks demonstrate an important point:

> A custom memory pool is not automatically faster than the system allocator in every workload.

The first benchmark favors the pool's block-based allocation strategy:

```text
Large Block
    |
    +-- Slot
    +-- Slot
    +-- Slot
    +-- Slot
```

Once a block exists, new slots can be obtained through relatively simple pointer advancement.

However, the immediate-reuse benchmark repeatedly exercises:

```text
allocate()
    |
    v
popFreeList()
    |
    v
atomic load
    |
    v
CAS


deallocate()
    |
    v
pushFreeList()
    |
    v
atomic load
    |
    v
CAS
```

Therefore, every iteration pays synchronization costs even though the benchmark itself is single-threaded.

Modern system allocators are also highly optimized for small, repeated allocations, making this workload particularly competitive.

The results therefore suggest that the current memory pool is workload-dependent:

| Workload | MemoryPool | System Allocator | Result |
|---|---:|---:|---|
| Batch allocation/deallocation | 3073 ms | 3562 ms | MemoryPool ~1.16x faster |
| Immediate allocate/deallocate | 3084 ms | 2604 ms | MemoryPool slower |

---

## Current Limitations

This project is primarily an educational memory allocator rather than a production-ready general-purpose allocator.

Current limitations include:

- The free list uses atomic CAS even in single-threaded workloads, introducing synchronization overhead.
- Block allocation still requires a mutex.
- Performance has currently been tested primarily with 32-byte allocations.
- Benchmark results are workload-dependent.
- More repeated benchmark runs are required for statistically reliable measurements.
- Multi-threaded contention has not yet been benchmarked.
- The current lock-free free-list design should be examined further for classic lock-free data-structure issues such as the ABA problem.
- The allocator does not attempt to replace a complete production system allocator.

---

## Planned Experiments

Future benchmarks should compare:

```text
1. Single-thread batch allocation/deallocation
2. Single-thread immediate reuse
3. Multi-thread batch allocation/deallocation
4. Multi-thread immediate reuse
```

Different slot sizes should also be tested:

```text
8 B
16 B
32 B
64 B
128 B
256 B
512 B
```

A particularly useful experiment would compare the current CAS free list with a non-atomic single-threaded free list.

This would help quantify the actual cost of:

```cpp
std::atomic
compare_exchange_weak
acquire/release ordering
```

independently from the rest of the memory-pool design.

---

## Build

The project uses the following layout:

```text
include/
    MemoryPool.h

src/
    MemoryPool.cpp
    main.cpp
```

When using MSVC directly, both `.cpp` files must be compiled and linked:

```bash
cl /EHsc /std:c++17 /O2 /Iinclude src\main.cpp src\MemoryPool.cpp
```

For benchmarking, a Release x64 build is recommended.

Debug builds should not be used for allocator performance comparisons because compiler optimizations and runtime checks can significantly affect the results.

---

## Key Concepts Explored

This project is intended as a practical study of:

- C++ raw memory management
- `operator new` / `operator delete`
- Placement new
- Pointer arithmetic
- Memory alignment
- Fixed-size allocation
- Memory pools
- Free lists
- Atomic pointers
- Compare-And-Swap (CAS)
- `compare_exchange_weak`
- Acquire/release memory ordering
- Mutexes and RAII
- Lock-free data structures
- Size-class based allocation
- Allocator benchmarking

---

## Conclusion

The current implementation demonstrates that memory-pool performance depends strongly on allocation patterns.

For batch allocation of 32-byte slots, the custom allocator showed approximately a **1.16x speedup** over direct system allocation in the measured run.

For repeated immediate allocation and deallocation, however, the custom allocator was approximately **18% slower**, largely because the fast reuse path performs atomic and CAS operations on every allocation and deallocation.

These results highlight the central engineering trade-off of the project:

> Reducing general-purpose allocation overhead through specialized memory reuse can improve performance, but synchronization and concurrency mechanisms introduce their own costs.

The next stage of the project is therefore to evaluate whether the atomic/CAS free-list design provides a meaningful advantage under multi-threaded contention.
