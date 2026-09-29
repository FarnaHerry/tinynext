#pragma once

// Small move-only owners for native resources used by the C++ modules. Include
// this from a module's global module fragment, after its platform headers.

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tinynext::native {

#ifdef _WIN32

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept { reset(value); }
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    explicit operator bool() const noexcept { return value_ != nullptr; }
    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept {
        HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (value == INVALID_HANDLE_VALUE) value = nullptr;
        if (value_) ::CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

class UniqueModule {
public:
    UniqueModule() = default;
    explicit UniqueModule(HMODULE value) noexcept : value_(value) {}
    ~UniqueModule() { reset(); }

    UniqueModule(const UniqueModule&) = delete;
    UniqueModule& operator=(const UniqueModule&) = delete;

    UniqueModule(UniqueModule&& other) noexcept : value_(other.release()) {}
    UniqueModule& operator=(UniqueModule&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    explicit operator bool() const noexcept { return value_ != nullptr; }
    HMODULE get() const noexcept { return value_; }
    HMODULE release() noexcept {
        HMODULE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HMODULE value = nullptr) noexcept {
        if (value_) ::FreeLibrary(value_);
        value_ = value;
    }

private:
    HMODULE value_ = nullptr;
};

class UniqueLocalAlloc {
public:
    UniqueLocalAlloc() = default;
    explicit UniqueLocalAlloc(HLOCAL value) noexcept : value_(value) {}
    ~UniqueLocalAlloc() { reset(); }

    UniqueLocalAlloc(const UniqueLocalAlloc&) = delete;
    UniqueLocalAlloc& operator=(const UniqueLocalAlloc&) = delete;

    UniqueLocalAlloc(UniqueLocalAlloc&& other) noexcept : value_(other.release()) {}
    UniqueLocalAlloc& operator=(UniqueLocalAlloc&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    explicit operator bool() const noexcept { return value_ != nullptr; }
    HLOCAL get() const noexcept { return value_; }
    HLOCAL release() noexcept {
        HLOCAL value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HLOCAL value = nullptr) noexcept {
        if (value_) ::LocalFree(value_);
        value_ = value;
    }

private:
    HLOCAL value_ = nullptr;
};

class UniqueIcon {
public:
    UniqueIcon() = default;
    explicit UniqueIcon(HICON value) noexcept : value_(value) {}
    ~UniqueIcon() { reset(); }

    UniqueIcon(const UniqueIcon&) = delete;
    UniqueIcon& operator=(const UniqueIcon&) = delete;

    UniqueIcon(UniqueIcon&& other) noexcept : value_(other.release()) {}
    UniqueIcon& operator=(UniqueIcon&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    explicit operator bool() const noexcept { return value_ != nullptr; }
    HICON get() const noexcept { return value_; }
    HICON release() noexcept {
        HICON value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HICON value = nullptr) noexcept {
        if (value_) ::DestroyIcon(value_);
        value_ = value;
    }

private:
    HICON value_ = nullptr;
};

class UniqueCoTaskMem {
public:
    using FreeFn = void(WINAPI*)(void*);

    UniqueCoTaskMem() = default;
    UniqueCoTaskMem(void* value, FreeFn freeFn) noexcept : value_(value), freeFn_(freeFn) {}
    ~UniqueCoTaskMem() { reset(); }

    UniqueCoTaskMem(const UniqueCoTaskMem&) = delete;
    UniqueCoTaskMem& operator=(const UniqueCoTaskMem&) = delete;

    UniqueCoTaskMem(UniqueCoTaskMem&& other) noexcept
        : value_(other.release()), freeFn_(other.freeFn_) {}
    UniqueCoTaskMem& operator=(UniqueCoTaskMem&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.release();
            freeFn_ = other.freeFn_;
        }
        return *this;
    }

    void* get() const noexcept { return value_; }
    void* release() noexcept {
        void* value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(void* value = nullptr) noexcept {
        if (value_ && freeFn_) freeFn_(value_);
        value_ = value;
    }

private:
    void* value_ = nullptr;
    FreeFn freeFn_ = nullptr;
};

class UniqueProcThreadAttributeList {
public:
    UniqueProcThreadAttributeList() = default;
    ~UniqueProcThreadAttributeList() { reset(); }

    UniqueProcThreadAttributeList(const UniqueProcThreadAttributeList&) = delete;
    UniqueProcThreadAttributeList& operator=(const UniqueProcThreadAttributeList&) = delete;

    bool initialize(HANDLE* handles, DWORD count) noexcept {
        reset();
        if (!handles || count == 0) return false;
        SIZE_T bytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0) return false;
        storage_ = ::HeapAlloc(::GetProcessHeap(), 0, bytes);
        if (!storage_) return false;
        list_ = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_);
        if (!::InitializeProcThreadAttributeList(list_, 1, 0, &bytes)) {
            reset();
            return false;
        }
        initialized_ = true;
        if (!::UpdateProcThreadAttribute(
                list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                sizeof(HANDLE) * count, nullptr, nullptr)) {
            reset();
            return false;
        }
        return true;
    }

    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    void reset() noexcept {
        if (initialized_) ::DeleteProcThreadAttributeList(list_);
        initialized_ = false;
        list_ = nullptr;
        if (storage_) ::HeapFree(::GetProcessHeap(), 0, storage_);
        storage_ = nullptr;
    }

    void* storage_ = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    bool initialized_ = false;
};

#else

class UniqueFd {
public:
    UniqueFd() = default;
    // Owned descriptors are close-on-exec by default; a spawn action can still
    // duplicate a descriptor onto stdout/stderr when the child needs it.
    explicit UniqueFd(int value) noexcept { reset(value); }
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : value_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    explicit operator bool() const noexcept { return value_ >= 0; }
    int get() const noexcept { return value_; }
    int release() noexcept {
        const int value = value_;
        value_ = -1;
        return value;
    }
    void reset(int value = -1) noexcept {
        if (value_ >= 0) ::close(value_);
        value_ = value;
        if (value_ >= 0) {
            const int flags = ::fcntl(value_, F_GETFD);
            if (flags < 0 || ::fcntl(value_, F_SETFD, flags | FD_CLOEXEC) < 0) {
                ::close(value_);
                value_ = -1;
            }
        }
    }

private:
    int value_ = -1;
};

#endif

class UniqueFile {
public:
    using CloseFn = int (*)(std::FILE*);

    UniqueFile() = default;
    UniqueFile(std::FILE* value, CloseFn closeFn) noexcept
        : value_(value), closeFn_(closeFn) {}
    ~UniqueFile() { reset(); }

    UniqueFile(const UniqueFile&) = delete;
    UniqueFile& operator=(const UniqueFile&) = delete;

    UniqueFile(UniqueFile&& other) noexcept
        : value_(other.release()), closeFn_(other.closeFn_) {}
    UniqueFile& operator=(UniqueFile&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.release();
            closeFn_ = other.closeFn_;
        }
        return *this;
    }

    explicit operator bool() const noexcept { return value_ != nullptr; }
    std::FILE* get() const noexcept { return value_; }
    std::FILE* release() noexcept {
        std::FILE* value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(std::FILE* value = nullptr) noexcept {
        if (value_ && closeFn_) closeFn_(value_);
        value_ = value;
    }

private:
    std::FILE* value_ = nullptr;
    CloseFn closeFn_ = nullptr;
};

} // namespace tinynext::native
