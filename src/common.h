#pragma once
// Shared includes and small RAII owners. Every Windows resource the app holds
// lives in one of these, so nothing has to be freed by hand.
// (UNICODE and NOMINMAX are defined for every file in CMakeLists.txt.)

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Owns one reference to a COM object.
template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Com& operator=(Com o) noexcept { std::swap(p_, o.p_); return *this; }

    void reset() { if (p_) std::exchange(p_, nullptr)->Release(); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** put() { reset(); return &p_; }

    template <class U>
    Com<U> as() const {
        Com<U> r;
        if (p_) p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(r.put()));
        return r;
    }

private:
    T* p_ = nullptr;
};

struct CoTaskMemDeleter {
    void operator()(void* p) const { CoTaskMemFree(p); }
};
using Pidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, CoTaskMemDeleter>;

struct MenuDeleter {
    void operator()(HMENU m) const { DestroyMenu(m); }
};
using Menu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;

struct FontDeleter {
    void operator()(HFONT f) const { DeleteObject(f); }
};
using Font = std::unique_ptr<std::remove_pointer_t<HFONT>, FontDeleter>;

// The app's name: window title, settings file, logs. (The exe name is set in CMakeLists.txt.)
constexpr wchar_t kAppName[] = L"Qoqdir";

inline int scale(int px, UINT dpi) { return MulDiv(px, static_cast<int>(dpi), 96); }
