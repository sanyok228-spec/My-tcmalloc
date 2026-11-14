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
#include<algorithm>
#include<iterator>
#include<array>
#include<deque>
#include<set>
#include<map>
#include <cctype>
#include <numeric>
#include <functional>
#include<cassert>
#include<thread>
#include<mutex>
#include<chrono>
#include<stdexcept>
#include<atomic>
#include<memory_resource>


// ─── Платформенная обёртка для выделения памяти ─── (чтобы функции os_alloc и os_free работали и для windows и для Linux)
#if defined(_WIN32)
#include <windows.h>

inline void* os_alloc(std::size_t size, bool use_huge_pages = false) {
    return VirtualAlloc(nullptr, size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
}

inline void os_free(void* ptr) {
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}
#else
#include <sys/mman.h>

inline void* os_alloc(std::size_t size, bool use_huge_pages = false) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (use_huge_pages) madvise(p, size, MADV_HUGEPAGE);
    return p;
}

inline void os_free(void* ptr) {
    if (ptr) munmap(ptr, 0);   // для учебной задачи — size не нужен
}
#endif




//для получения NUMA - узла (на один CPU один NUMA - узел)
#if defined(_WIN32) //для винды
#include <windows.h>

// Возвращает NUMA-узел текущего потока (0 если не удалось)
inline unsigned current_numa_node() {
    PROCESSOR_NUMBER procNum;
    GetCurrentProcessorNumberEx(&procNum);

    USHORT nodeNumber = 0;
    if (!GetNumaProcessorNodeEx(&procNum, &nodeNumber)) {
        return 0;   // fallback
    }
    return nodeNumber;
}
#else //для линукса
#include <numa.h>   // libnuma
inline unsigned current_numa_node() {
    int cpu = sched_getcpu();
    int node = numa_node_of_cpu(cpu);
    return node < 0 ? 0 : static_cast<unsigned>(node);
}
#endif






//тот же os_alloc, только с NUMA - node
#if defined(_WIN32)
inline void* os_alloc_on_node(std::size_t size, unsigned node) {
    return VirtualAllocExNuma(
        GetCurrentProcess(),
        nullptr,
        size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE,
        node);   // ← номер NUMA-узла
}
#else
inline void* os_alloc_on_node(std::size_t size, unsigned node) {
    // libnuma: numa_alloc_onnode выделяет на узле
    void* p = numa_alloc_onnode(size, node);
    if (p && size >= 2 * 1024 * 1024) {
        // huge pages на Linux для numa нужны отдельно — упрощаем
    }
    return p;
}
#endif










struct FreeNode { FreeNode* next; }; //структура узла


//взят для примера
constexpr std::size_t kSizeClasses[] = {
    8, 16, 32, 48, 64, 80, 96, 112, 128,
    192, 256, 320, 384, 448, 512,
    768, 1024, 1280, 1536, 1792, 2048,
};



//хранит в себе freeList и узлы в нём(сам ничего не выделяет - только хранит)
class SlabCache {
public:
    //запрет копирования
    SlabCache(const SlabCache&) = delete;
    SlabCache& operator=(const SlabCache&) = delete;

    //конструктор по умолчанию
    SlabCache() = default;

    //конструктор с параметрами
    explicit SlabCache(std::size_t blockSize) {
        blockSize_ = (std::max)(blockSize, sizeof(FreeNode*));
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










//регион(регион - это фактически структура арены только у которой будет расти размер в байтах геометрически при нехватки предыдущей арены)     (для использования в CentralCache)
struct Region {
    char* base = nullptr;          // начало (::operator new(size))
    std::size_t size = 0;    // размер в байтах (кратен 4096)
    std::size_t offset = 0;  // сколько уже занято(сначала всегда 0, если еще не брали память)
    std::vector<std::size_t> pageMap;  // размер size / 4096, все 255
    unsigned numa_node = 0; // NUMA - node
};











//один общий для всех потоков
class CentralCache {
public:
    //конструктор
    CentralCache(){
        
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
    CentralCache(std::size_t bytes) : nextRegionSize_(bytes) {

        caches_.reserve(21);
        sizeToClass_.fill(255);

        //заполняем вектор unique_ptr'ами на SlabCache'и по классам из kSizeClasses
        for (std::size_t i = 0; i < 21; ++i) {
            caches_.push_back(std::make_unique<SlabCache>(kSizeClasses[i]));
        }


        for (std::size_t i = 0; i < 21; ++i) {
            if (kSizeClasses[i] > 256) break; //выходим из цикла если kSizeClasses[i] > 256 байт т.к. доступ за время O(1) работает только с классами c size <= 256 байт

            std::size_t prev = (i == 0) ? 0 : kSizeClasses[i - 1];

            for (std::size_t s = prev + 1; s <= kSizeClasses[i]; s++) { //перебираем все байты от kSizeClasses[i - 1] + 1 до kSizeClasses[i] чтобы добавить все промежуточные значения байтов
                if (s < sizeToClass_.size()) sizeToClass_[s] = i;
            }
        }
    }

    //деструктор удаляющий всю выделенную память(в цикле проходимся по всем регионам и удаляем их арены)
    ~CentralCache() { 
        for (auto& r : regions_) {
            os_free(r.base);
        }
    }


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
                void* slab = nullptr;
                {
                    std::lock_guard<std::mutex>arenaLock(arenaMutex_);
                    slab = bumpAlloc(slabSize); //выделяем чанк памяти
                    if (slab) markPages(idx, slab, slabSize); //заполняем страницы
                }

                if (!slab) return nullptr; //если арена кончилась то выходим из функции(возвращаем nullptr)

                caches_[idx]->addSlab(slab); //разбиваем ранее выделенный чанк на блоки 
                continue;
            }

            FreeNode* node = static_cast<FreeNode*>(p); //кастуем указатель к узлу FreeNode
            node->next = nullptr; //указатель на следующий элемент после node = nullptr т.к. мы добавляем его в конец списка

            if (!head) { //если список пустой(добавление первого элемента)
                head = node;
                tail = node;
            }
            else { //tail списка указывает на node как на следующий элемент и tail сдвигается
                static_cast<FreeNode*>(tail)->next = node;
                tail = node;
            }
            ++taken; //увеличиваем счётчик(чтобы выделить ровно count элементов)

        }

        
        return head; //возвращаем голову списка с count элементами

    }

    //освобождаем list с помощью caches_[idx]
    void deallocateBatch(std::size_t idx, void* list) {
        if (!list) return;  //выходим из функции если list пуст(освобождать нечего)
        std::lock_guard<std::mutex>lock(mutexes_[idx]); //для потокобезопасности
        FreeNode* cur = static_cast<FreeNode*>(list); //временный указатель для движения по списку в цикле
        while (cur) { //в цикле по очереди освобождаем каждый элемент list
            FreeNode* next = cur->next;
            caches_[idx]->deallocate(cur);
            cur = next;
        }
    }


    //возвращает индекс для нужного caches_[ind] по указателю p
    std::size_t classOfPointer(void* p) {
        char* pc = static_cast<char*>(p); //начало указателя p (адрес памяти)

        // binary search: найти последний регион, у которого base <= pc
        auto it = std::upper_bound(regions_.begin(), regions_.end(), pc,
            [](char* p, const Region& r) { return p < r.base; });

        if (it == regions_.begin()) return 255;   // pc меньше первого региона
        --it;                                     // сдвигаем указатель т.к. нам нужен предыдущий регион

        const Region& r = *it; //найденный нами регион
        if (pc < r.base || pc >= r.base + r.size) return 255;   // вне региона(адрес pc < начало арены региона или начало арены региона + её размер < адреса pc)

        std::size_t pageInd = (pc - r.base) / 4096;
        if (pageInd >= r.pageMap.size()) return 255;
        if (r.pageMap[pageInd] == 255) return 255;
        return r.pageMap[pageInd];

    }

    //сколько байт использовано всего (offset - arena_ у каждого региона)
    std::size_t arenaUsed() const {
        std::size_t total = 0;
        for (auto& r : regions_) {
            total += r.offset;
        }
        return total;
    }


private:
    std::vector<Region> regions_;        // регионы в порядке добавления (отсортированы по base)
    std::size_t nextRegionSize_ = 16 * 1024 * 1024;  // следующий размер
    std::mutex arenaMutex_;              // защита regions_ + bumpAlloc + markPages
    std::vector<std::unique_ptr<SlabCache>> caches_; //классы SlabCache с разными размерами блоков
    std::array<std::size_t, 257> sizeToClass_; //по размеру в байтах(индексу) возвращает индекс для нужного класса в caches_
    std::array<std::mutex, 21> mutexes_; //массив мьютексов
    


    //выделяет один чанк памяти(4096 байт которые потом можно будет разбить на блоки с помощью slabCache_)
    void* bumpAlloc(std::size_t size) {
        size = (size + 4095) & ~std::size_t(4095); // выравниваем размер до страницы (4096)

        unsigned node = current_numa_node();   // ← узел текущего потока

        // пробуем взять из текущего региона
        for (auto& r : regions_) { //ищем подходящий регион(у которого хватает памяти и узел потока совпадает с текущим)
            if (r.offset + size <= r.size && r.numa_node == node) {
                void* ptr = r.base + r.offset;
                r.offset += size;
                return ptr;
            }
        }


        // если нужно добавить новый регион в вектор(вектор регионов пуст или не хватает памяти в регионе)
        std::size_t newSize = (std::max)(nextRegionSize_, size);

        char* base = static_cast<char*>(os_alloc_on_node(newSize, node));

        // Fallback: если NUMA не поддерживает этот узел — обычный os_alloc
        if (!base) base = static_cast<char*>(os_alloc(newSize));
        if (!base) return nullptr; //если не хватило памяти(на уровне системных вызовов)

        Region r;
        r.base = base;
        r.size = newSize;
        r.offset = size;                          // сразу заняли size (сразу сдвинули offset на size байт т.к. мы же их выделили)
        r.pageMap.assign(newSize / 4096, 255);    // все 255
        r.numa_node = node;

        regions_.push_back(std::move(r));

        nextRegionSize_ *= 2;                     // следующий регион вдвое больше (геометрический рост)

        return base;
    }

    //добавляет новую страницу
    void markPages(std::size_t idx, void* slab, std::size_t slabSize) {
        char* sc = static_cast<char*>(slab);
        for (auto& r : regions_) {
            if (sc >= r.base && sc < r.base + r.size) {
                std::size_t startPage = (sc - r.base) / 4096;
                std::size_t pageCount = slabSize / 4096;
                for (std::size_t i = 0; i < pageCount; ++i)
                    r.pageMap[startPage + i] = idx;
                return;
            }
        }
        // не должно случиться — slab выделен из региона
        assert(false && "slab not in any region");
    }

};






//у каждого потока свой TreadCache, но все они используют общий CentralCache
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

        std::size_t count = 0; //переменная счетчик для подсчета количества элементов в списке localFreeLists_[idx]
        for (FreeNode* cur = batch; cur; cur = cur->next) ++count; //движемся по списку в цикле и считаем элементы
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
        if (idx == 255) { //если индекса idx нет в PageMap_ который находится в central_
            ::operator delete(p); 
            return;
        }

        FreeNode* node = static_cast<FreeNode*>(p); //кастуем указатель к FreeNode*
        ++localCount_[idx]; //увеличение счетчика количества элементов списка


        if (localCount_[idx] > kMaxLocal) { //если превышение по количеству элементов в списке, то отрезаем kBatchSize элементов и освобождаем их память
            FreeNode* head = localFreeLists_[idx];
            FreeNode* cur = localFreeLists_[idx];
            for (std::size_t i = 0; i < kBatchSize - 1; ++i) { //двигаем указатель в цикле чтобы потом отсоединить этот кусок списка
                if (!cur) break;
                cur = cur->next;
            }
            if (cur) {
                FreeNode* rest = cur->next; //новая голова списка(а точнее второй половины списка)
                cur->next = nullptr; //обнуляем указатель cur чтобы он не указывал на оставшуюся часть списка(для избежания багов)
                central_->deallocateBatch(idx, head); //освобождаем тот кусок который мы отрезали
                node->next = rest; //прицепляем тот указатель который нам передали в параметрах
                localFreeLists_[idx] = node; //теперь делаем его головой списка
                localCount_[idx] -= kBatchSize; //уменьшаем счетчик количества элементов списка
            }
        }
        else { //в остальных случаях просто прицепляем указатель и всё
            node->next = localFreeLists_[idx];
            localFreeLists_[idx] = node;
        }
    }



private:
    CentralCache* central_; // центральный аллокатор(общий для всех потоков)
    std::array<FreeNode*, 21> localFreeLists_{}; //массив списков с узлами(для каждого класса свой)
    std::array<std::size_t, 21> localCount_{}; //количество элементов в списке каждого класса(счетчики количества элементов)

    static constexpr std::size_t kBatchSize = 32; //сколько элементов выделяем при выделении батча
    static constexpr std::size_t kMaxLocal = 64; //сколько максимум элементов может быть в списке localFreeLists_(для всех классов одинаково)
};








CentralCache globalCentral; //общий для всех потоков
thread_local ThreadCache tlsCache(&globalCentral); //у каждого потока свой, но все они внутри используют общий globalCentral

void* my_alloc(std::size_t s) { return tlsCache.allocate(s); } //выделение памяти(функция сделана для удобства и для тестов)

void  my_free(void* p) { tlsCache.deallocate(p); } //освобождение памяти(функция сделана для удобства и для тестов)












//обёртка для STL контейнеров
class MiniTcmallocResource : public std::pmr::memory_resource {
public:
    //конструктор
    explicit MiniTcmallocResource(std::size_t arenaBytes = 16 * 1024 * 1024) : central_(arenaBytes) {}

    //деструктор
    ~MiniTcmallocResource() = default;

    // Запрет копирования (memory_resource некопируемый)
    MiniTcmallocResource(const MiniTcmallocResource&) = delete;
    MiniTcmallocResource& operator=(const MiniTcmallocResource&) = delete;

    std::size_t used() const {
        return used_.load();
    }

private:
    //выделение памяти
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        thread_local ThreadCache tls(&central_);//для каждого потока создаётся свой ThreadCache, который привяжется к этому потоку и в след.раз поток просто использует уже созданный ThreadCache
        ++used_;
        return tls.allocate(bytes);
    }

    //освобождение памяти
    void  do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        thread_local ThreadCache tls(&central_); //для каждого потока создаётся свой ThreadCache, который привяжется к этому потоку и в след.раз поток просто использует уже созданный ThreadCache
        --used_;
        tls.deallocate(p);
    }

    //сравнение двух ресурсов
    bool  do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    CentralCache central_; //центральный кэш(один на все потоки)
    std::atomic<std::size_t> used_ = 0; //атомарный счетчик использованных узлов(без мьютексов быстрее)
};

