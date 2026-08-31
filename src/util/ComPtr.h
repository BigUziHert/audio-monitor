#pragma once
//
// Minimal intrusive COM smart pointer.
//
// WRL::ComPtr and _com_ptr_t are both MSVC-flavoured; this is a few lines and
// keeps the tree building unmodified under mingw for CI.
//
#include <utility>

namespace audiomon {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }

    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }

    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }

    T*  get()        const { return p_; }
    T*  operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // For the ...(&ptr) out-parameter idiom. Releases any current value first
    // so a retry loop cannot leak.
    T** put() { reset(); return &p_; }
    void** putVoid() { reset(); return reinterpret_cast<void**>(&p_); }

    T* detach() { T* t = p_; p_ = nullptr; return t; }
    void attach(T* t) { reset(); p_ = t; }

private:
    T* p_ = nullptr;
};

} // namespace audiomon
