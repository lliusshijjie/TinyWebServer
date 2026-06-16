#pragma once

#include <sys/mman.h>
#include <cstddef>

// RAII wrapper for mmap/munmap. Move-only; not copyable.
class MmapGuard {
public:
    MmapGuard() = default;

    MmapGuard(void* addr, std::size_t size) noexcept
        : addr_(static_cast<char*>(addr)), size_(size) {}

    ~MmapGuard() { release(); }

    MmapGuard(const MmapGuard&) = delete;
    MmapGuard& operator=(const MmapGuard&) = delete;

    MmapGuard(MmapGuard&& o) noexcept : addr_(o.addr_), size_(o.size_) {
        o.addr_ = nullptr;
        o.size_ = 0;
    }

    MmapGuard& operator=(MmapGuard&& o) noexcept {
        if (this != &o) {
            release();
            addr_ = o.addr_;
            size_ = o.size_;
            o.addr_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }

    char*       get()   const noexcept { return addr_; }
    std::size_t size()  const noexcept { return size_; }
    bool        valid() const noexcept { return addr_ != nullptr; }

    void release() noexcept {
        if (addr_) {
            munmap(addr_, size_);
            addr_ = nullptr;
            size_ = 0;
        }
    }

private:
    char*       addr_ = nullptr;
    std::size_t size_ = 0;
};
