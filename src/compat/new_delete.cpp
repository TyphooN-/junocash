#include <new>
#include <cstdlib>

void* operator new(std::size_t size) {
    if (void* ptr = std::malloc(size))
        return ptr;
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void* operator new[](std::size_t size) {
    if (void* ptr = std::malloc(size))
        return ptr;
    throw std::bad_alloc();
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}
