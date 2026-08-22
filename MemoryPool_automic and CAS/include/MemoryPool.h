#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>

namespace nicolia_memorypool{
    #define MEMORY_POOL_NUM 64 // 512/8=64 memory pool has 64 slot
    #define SLOT_BASE_SIZE 8 //minimal slot size is 8 bytes
    #define MAX_SLOT_SIZE 512 //memory pool maximal handle 512 bytes size slot

    struct Slot{
        std::atomic<Slot*> next;
    };

    class MemoryPool{
    public:
        MemoryPool(size_t BlockSize=4096);//Each MemoryPool is 4096 bytes
        ~MemoryPool();

        void init(size_t);//Configeration of Slot size eg. init(8) 8 byte slot

        void* allocate();// get memory from memorypool, void* because OS doesn't care which type of data will be stored
        void deallocate(void*);

    private:
        void allocateNewBlock();//current block run out, apply for new one
        size_t padPointer(char* p,size_t align);//calcute how many bytes should move to align

        bool pushFreeList(Slot* slot);//push a free slot into freelist
        Slot* popFreeList();//get an available free slot from freelist

    private:
        int  BlockSize_;
        int  SlotSize_;
        Slot* firstBlock_;// point to first block into blocklist
        Slot* curSlot_;// point to next available free slot
        std:: atomic<Slot*> freeList_;//curslot point to which one has been never allocated,freelist point to those are relaeased.We consider allocate freelist at first,if freelist==npr, then we allocate curslot
        Slot* lastSlot_;//the laswt slot in this block, if(curslot>lastslot) currentblock is full

        std::mutex MutexForBlock_;//avoid both two threads apply for new block at the same time
        
    };

    class HashBucket{
    public:
        static void initMemoryPool();
        static MemoryPool& getMemoryPool(int index);

        static void* useMemory(size_t size){
            if(size <=0)
                return nullptr;
            
            if(size > MAX_SLOT_SIZE)
                return operator new(size);
            
            return getMemoryPool(
                ((size+ SLOT_BASE_SIZE - 1)/SLOT_BASE_SIZE)-1
            ).allocate();
        }

        static void freeMemory(void* ptr,size_t size){
            if(!ptr)
                return;
            
            if(size > MAX_SLOT_SIZE){
                operator delete(ptr);
                return;
            }

            getMemoryPool(
                ((size+ SLOT_BASE_SIZE - 1)/SLOT_BASE_SIZE)-1
            ).deallocate(ptr);
        }
        
        template<typename T,typename... Args>
        friend T* newElement(Args&&... args);

        template<typename T>
        friend void deleteElement(T* ptr);

    };

    template<typename T,typename... Args>
    T* newElement(Args&&... args){
        T* ptr = nullptr;
        ptr=reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)));
        if(ptr!=nullptr){
            new(ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    };

    template<typename T>
    void deleteElement(T* ptr){
        if(ptr!=nullptr){
            ptr->~T();
            HashBucket::freeMemory(ptr,sizeof(T));
        }
    };


}