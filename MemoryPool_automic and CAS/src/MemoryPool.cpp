#include "MemoryPool.h"

namespace nicolia_memorypool
{

    MemoryPool::MemoryPool(size_t BlockSize)
        : BlockSize_(BlockSize),
        SlotSize_(0),
        firstBlock_(nullptr),
        curSlot_(nullptr),
        freeList_(nullptr),
        lastSlot_(nullptr)
    {
    }


   
    MemoryPool::~MemoryPool()
    {
        Slot* currentBlock = firstBlock_;

        while (currentBlock != nullptr)
        {
            // Save next Block before deleting current Block.
            Slot* nextBlock =
                currentBlock->next.load(
                    std::memory_order_relaxed
                );

            operator delete(
                reinterpret_cast<void*>(currentBlock)
                );

            currentBlock = nextBlock;
        }
    }


    void MemoryPool::init(size_t size)
    {
        assert(size > 0);

        SlotSize_ = static_cast<int>(size);

        firstBlock_ = nullptr;
        curSlot_ = nullptr;
        freeList_.store(nullptr, std::memory_order_relaxed);
        lastSlot_ = nullptr;
    }

    size_t MemoryPool::padPointer(char* p, size_t align)
    {
        size_t rem =
            reinterpret_cast<size_t>(p) % align;

        return rem == 0
            ? 0
            : align - rem;
    }


 
    void MemoryPool::allocateNewBlock()
    {
        // Request raw memory from the system.
        void* newBlock =
            operator new(BlockSize_);



        reinterpret_cast<Slot*>(newBlock)
            ->next.store(
                firstBlock_,
                std::memory_order_relaxed
            );

        firstBlock_ =
            reinterpret_cast<Slot*>(newBlock);

        char* body =
            reinterpret_cast<char*>(newBlock)
            + sizeof(Slot*);


        // Add alignment padding.
        size_t bodyPadding =
            padPointer(
                body,
                static_cast<size_t>(SlotSize_)
            );


        // First never-used Slot.
        curSlot_ =
            reinterpret_cast<Slot*>(
                body + bodyPadding
                );


        // Last Slot whose full SlotSize_ bytes still fit
        // inside this Block.
        lastSlot_ =
            reinterpret_cast<Slot*>(
                reinterpret_cast<char*>(newBlock)
                + BlockSize_
                - SlotSize_
                );
    }


    void* MemoryPool::allocate()
    {
        // --------------------------------------------------------
        // First reuse a previously released Slot.
        // --------------------------------------------------------

        Slot* slot = popFreeList();

        if (slot != nullptr)
        {
            return slot;
        }


        Slot* temp = nullptr;



        {
            std::lock_guard<std::mutex> lock(MutexForBlock_);


            // No Block yet, or current Block is exhausted.
            if (curSlot_ == nullptr || curSlot_ > lastSlot_)
            {
                allocateNewBlock();
            }


            // Save the Slot that will be returned.
            temp = curSlot_;



            curSlot_ +=
                SlotSize_ / sizeof(Slot);
        }


        return temp;
    }

    void MemoryPool::deallocate(void* ptr)
    {
        if (ptr == nullptr)
        {
            return;
        }


        Slot* slot =
            reinterpret_cast<Slot*>(ptr);


        pushFreeList(slot);
    }


   
    bool MemoryPool::pushFreeList(Slot* slot)
    {
        Slot* oldHead =
            freeList_.load(
                std::memory_order_relaxed
            );


        do
        {
            // New Slot points to the current head.
            slot->next.store(
                oldHead,
                std::memory_order_relaxed
            );


            // CAS attempts:
            //
            // if (freeList_ == oldHead)
            //     freeList_ = slot;
            //
            // If CAS fails, oldHead is updated automatically
            // with the newest freeList_ value.
        } while (
            !freeList_.compare_exchange_weak(
                oldHead,
                slot,
                std::memory_order_release,
                std::memory_order_relaxed
            )
            );


        return true;
    }


    Slot* MemoryPool::popFreeList()
    {
        Slot* oldHead =
            freeList_.load(
                std::memory_order_acquire
            );


        while (oldHead != nullptr)
        {
            Slot* newHead =
                oldHead->next.load(
                    std::memory_order_relaxed
                );


            // Try to replace A with B.
            if (
                freeList_.compare_exchange_weak(
                    oldHead,
                    newHead,
                    std::memory_order_acquire,
                    std::memory_order_relaxed
                )
                )
            {
                return oldHead;
            }


            // If CAS fails, oldHead has already been updated
            // to the current freeList_ head.
        }


        return nullptr;
    }

    void HashBucket::initMemoryPool()
    {
        for (int i = 0; i < MEMORY_POOL_NUM; ++i)
        {
            getMemoryPool(i).init(
                (i + 1) * SLOT_BASE_SIZE
            );
        }
    }


    MemoryPool& HashBucket::getMemoryPool(int index)
    {
        static MemoryPool memoryPool[MEMORY_POOL_NUM];

        return memoryPool[index];
    }


} // namespace nicolia_memorypool