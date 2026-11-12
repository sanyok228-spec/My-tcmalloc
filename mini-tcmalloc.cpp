#include <optional>
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <cmath>
#include<locale.h>
#include<fstream>
#include<limits>
#include<utility>
#include<memory>
#include "httplib.h"
#include<algorithm>
#include<iterator>
#include<list>
#include<forward_list>
#include<array>
#include<deque>
#include<set>
#include<map>
#include <cctype>
#include <numeric>
#include<stack>
#include<queue>
#include<unordered_map>
#include <functional>
#include<cassert>
#include<thread>
#include<mutex>
#include<chrono>
#include<stdexcept>
#include<atomic>
#include <condition_variable>
#include<memory_resource>


struct FreeNode { FreeNode* next; }; //структура узла



constexpr std::size_t kSizeClasses[] = {
    8, 16, 32, 48, 64, 80, 96, 112, 128,
    192, 256, 320, 384, 448, 512,
    768, 1024, 1280, 1536, 1792, 2048,
};



//типо пула только с принимаемым размеров блоков
class SlabCache {
public:
    //запрет копирования
    SlabCache(const SlabCache&) = delete;
    SlabCache& operator=(const SlabCache&) = delete;

    //конструктор по умолчанию
    SlabCache() = default;

    //конструктор с параметрами
    explicit SlabCache(std::size_t blockSize) {
        blockSize_ = std::max(blockSize, sizeof(FreeNode*));
        assert(blockSize_ <= slabSize_);
    }

    //выделение памяти
    void* allocate() {     
        if (freeList_ == nullptr) return nullptr;
        FreeNode* node = freeList_;
        freeList_ = node->next;
        void* ptr = static_cast<void*>(node);
        return ptr;
    }
    //освобождение памяти
    void deallocate(void* p) {
        FreeNode* node = static_cast<FreeNode*>(p);
        node->next = freeList_;
        freeList_ = node;
    }


    std::size_t slabSize() const { return slabSize_; }

    std::size_t blockSize() const { return blockSize_; }

    //принимает кусок памяти и разбивает ее на блоки и кладет их в freeList_
    void addSlab(void* raw) {
        char* start = static_cast<char*>(raw);
        std::size_t Count = slabSize_ / blockSize_;
        for (std::size_t i = 0; i < Count; i++) {
            FreeNode* node = reinterpret_cast<FreeNode*>(start + i * blockSize_);
            node->next = freeList_;
            freeList_ = node;
        }
    }

private:

    std::size_t blockSize_ = 0; //размер одного блока
    std::size_t slabSize_ = 4096;  //размер одного чанка
    FreeNode* freeList_ = nullptr; //список с блоками памяти


    
};






class CentralCache {
public:
    //конструктор
    CentralCache()
        : arena_(static_cast<char*>(::operator new(arenaSize_)))
        , pageMap_(arenaSize_ / 4096, 255) {   // 255 = free/large

        caches_.reserve(21);
        sizeToClass_.fill(255);

        for (std::size_t i = 0; i < 21; ++i) {
            caches_.push_back(std::make_unique<SlabCache>(kSizeClasses[i]));
        }


        for (std::size_t i = 0; i < 21; ++i) {
            if (kSizeClasses[i] > 256) break;

            std::size_t prev = (i == 0) ? 0 : kSizeClasses[i - 1];

            for (std::size_t s = prev + 1; s <= kSizeClasses[i]; s++) {
                if (s < sizeToClass_.size()) sizeToClass_[s] = i;
            }

        }
    }

    //конструктор с параметром
    CentralCache(std::size_t bytes)
        : arenaSize_(bytes),
        arena_(static_cast<char*>(::operator new(arenaSize_)))
        , pageMap_(arenaSize_ / 4096, 255) {   // 255 = free/large

        caches_.reserve(21);
        sizeToClass_.fill(255);

        for (std::size_t i = 0; i < 21; ++i) {
            caches_.push_back(std::make_unique<SlabCache>(kSizeClasses[i]));
        }


        for (std::size_t i = 0; i < 21; ++i) {
            if (kSizeClasses[i] > 256) break;

            std::size_t prev = (i == 0) ? 0 : kSizeClasses[i - 1];

            for (std::size_t s = prev + 1; s <= kSizeClasses[i]; s++) {
                if (s < sizeToClass_.size()) sizeToClass_[s] = i;
            }

        }
    }

    //деструктор удаляющий всю выделенную память
    ~CentralCache() { ::operator delete(arena_); }


    //принимает размер блока в байтах а возвращает индекс (для дальнейшего использования)
    std::size_t classIndex(std::size_t size) const {
        if (size == 0) size = 1;
        if (size <= 256) return sizeToClass_[size];


        auto res = std::lower_bound(std::begin(kSizeClasses), std::end(kSizeClasses), size);
        if (res == std::end(kSizeClasses)) return SIZE_MAX;
        return static_cast<std::size_t>(res - std::begin(kSizeClasses));
    }


    //выделяем count элементов с помощью caches_[idx] и возвращаем голову списка
    void* allocateBatch(std::size_t idx, std::size_t count) {
        std::lock_guard<std::mutex>lock(mutexes_[idx]);
        void* head = nullptr;
        void* tail = nullptr;
        std::size_t taken = 0; //счетчик для цикла
        while (taken < count) {
            void* p = caches_[idx]->allocate();
            if (!p) { //если в caches_[idx] freeList_ пустой то мы выделяем новый чанк памяти
                std::size_t slabSize = caches_[idx]->slabSize();
                void* slab = bumpAlloc(slabSize); //выделяем чанк памяти
                if (!slab) return nullptr; //если арена кончилась то выходим из цикла

                caches_[idx]->addSlab(slab); //разбиваем ранее выделенный чанк на блоки 
                markPages(idx, slab, slabSize); //добавляем новую страницу
                continue;
            }

            FreeNode* node = static_cast<FreeNode*>(p); //кастуем указатель к узлу FreeNode
            node->next = nullptr;

            if (!head) {
                head = node;
                tail = node;
            }
            else {
                static_cast<FreeNode*>(tail)->next = node;
                tail = node;
            }
            ++taken;

        }

        
        return head; //возвращаем голову списка с count элементами

    }


    void deallocateBatch(std::size_t idx, void* list) {
        if (!list) return;
        std::lock_guard<std::mutex>lock(mutexes_[idx]);
        FreeNode* cur = static_cast<FreeNode*>(list);
        while (cur) {
            FreeNode* next = cur->next;
            caches_[idx]->deallocate(cur);
            cur = next;
        }
    }


    //возвращает индекс для нужного caches_[ind] по указателю p
    std::size_t classOfPointer(void* p) {
        std::size_t pageInd = (reinterpret_cast<char*>(p) - arena_) / 4096;
        if (pageInd >= pageMap_.size() || pageMap_[pageInd] == 255) {
            return 255;
        }
        return pageMap_[pageInd];
    }

    //геттер возвращающий оффсет арены
    std::size_t arenaUsed() const { return arenaOffset_; }


private:
    std::size_t arenaSize_ = 16 * 1024 * 1024; //размер памяти в байтах
    char* arena_ = nullptr; //память
    std::size_t arenaOffset_ = 0; //оффсет
    std::vector <std::size_t> pageMap_; //для работы со страницами
    std::vector<std::unique_ptr<SlabCache>> caches_; //классы SlabCache с разными размерами блоков
    std::array<std::size_t, 257> sizeToClass_; //по размеру в байтах(индексу) возвращает индекс для нужного класса в caches_
    std::array<std::mutex, 21> mutexes_; //массив мьютексов
    

    //выделяет один чанк памяти(4096 байт которые потом можно будет разбить на блоки с помощью slabCache_)
    void* bumpAlloc(std::size_t size) {
        if (arenaOffset_ + size > arenaSize_) return nullptr;
        void* ptr = arena_ + arenaOffset_;
        arenaOffset_ += size;
        return ptr;
    }

    //добавляет новую страницу
    void markPages(std::size_t idx, void* slab, std::size_t slabSize) {
        std::size_t startPage = (static_cast<char*>(slab) - arena_) / 4096;
        std::size_t pageCount = slabSize / 4096;
        for (std::size_t i = 0; i < pageCount; ++i)
            pageMap_[startPage + i] = idx;
    }

};







class ThreadCache {
public:
    //конструктор
    explicit ThreadCache(CentralCache* c) : central_(c) { }

    //деструктор
    ~ThreadCache() {
        for (std::size_t i = 0; i < 21; ++i) {
            if (localFreeLists_[i]) {
                central_->deallocateBatch(i, localFreeLists_[i]);
            }
        }
    }

    //выделение памяти
    void* allocate(std::size_t size) {
        std::size_t idx = central_->classIndex(size); //индекс нужного нам класса аллокатора
        if (idx == SIZE_MAX) return ::operator new(size);

        if (localFreeLists_[idx]) { //если список localFreeLists_[idx] не пуст просто берем узел
            FreeNode* node = localFreeLists_[idx];
            localFreeLists_[idx] = localFreeLists_[idx]->next;
            --localCount_[idx];
            return static_cast<void*>(node);
        }

        //выделяем новый батч если список localFreeLists_[idx] пуст
        FreeNode* batch = static_cast<FreeNode*>(central_->allocateBatch(idx, kBatchSize));
        if (!batch) throw std::bad_alloc(); //если арена кончилась бросаем исключение

        std::size_t count = 0;
        for (FreeNode* cur = batch; cur; cur = cur->next) ++count;
        localCount_[idx] = count;
        localFreeLists_[idx] = batch;

        //после выделения нового батча просто берем один узел списка
        FreeNode* node = localFreeLists_[idx];
        localFreeLists_[idx] = localFreeLists_[idx]->next;
        --localCount_[idx];
        return static_cast<void*>(node);
    }

    //освобождение памяти
    void deallocate(void* p) {
        std::size_t idx = central_->classOfPointer(p);
        if (idx == 255) {
            ::operator delete(p); 
            return;
        }

        FreeNode* node = static_cast<FreeNode*>(p);
        ++localCount_[idx];


        if (localCount_[idx] > kMaxLocal) { //если превышение по количеству элементов в списке, то отрезаем kBatchSize элементов и освобождаем их память
            FreeNode* head = localFreeLists_[idx];
            FreeNode* cur = localFreeLists_[idx];
            for (std::size_t i = 0; i < kBatchSize - 1; ++i) { //двигаем указатель в цикле чтобы потом отсоединить этот кусок списка
                if (!cur) break;
                cur = cur->next;
            }
            if (cur) {
                FreeNode* rest = cur->next;
                cur->next = nullptr;
                central_->deallocateBatch(idx, head);
                node->next = rest;
                localFreeLists_[idx] = node;
                localCount_[idx] -= kBatchSize;
            }
        }
        else {
            node->next = localFreeLists_[idx];
            localFreeLists_[idx] = node;
        }
    }



private:
    CentralCache* central_; // аллокатор
    std::array<FreeNode*, 21> localFreeLists_{}; //массив списков с узлами(для каждого класса свой)
    std::array<std::size_t, 21> localCount_{}; //количество элементов в списке каждого класса

    static constexpr std::size_t kBatchSize = 32; //сколько элементов выделяем при выделении батча
    static constexpr std::size_t kMaxLocal = 64; //сколько максимум элементов может быть в списке localFreeLists_(для всех классов одинаково)
};



CentralCache globalCentral; //общий для всех потоков
thread_local ThreadCache tlsCache(&globalCentral); //у каждого потока свой, но все они внутри используют общий globalCentral

void* my_alloc(std::size_t s) { return tlsCache.allocate(s); } //выделение памяти(функция сделана для удобства)

void  my_free(void* p) { tlsCache.deallocate(p); } //освобождение памяти(функция сделана для удобства)

