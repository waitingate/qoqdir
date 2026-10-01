#include "folder.h"

#include <algorithm>

namespace {

bool isSep(wchar_t c) { return c == L'\\' || c == L'/'; }
bool isDriveRoot(const std::wstring& p) { return p.size() == 3 && p[1] == L':' && p[2] == L'\\'; }
bool isUnc(const std::wstring& p) { return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\'; }

bool equalNoCase(const wchar_t* a, int an, const wchar_t* b, int bn) {
    return CompareStringOrdinal(a, an, b, bn, TRUE) == CSTR_EQUAL;
}

// Pointer to the extension inside name (".txt"), or to the terminating zero.
const wchar_t* extPtr(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name.c_str() + name.size() : name.c_str() + dot;
}

void listDrives(Listing& r) {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        Item it;
        it.name = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
        it.attrs = FILE_ATTRIBUTE_DIRECTORY;
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(it.name.c_str(), 0, &sfi, sizeof sfi,
                           SHGFI_DISPLAYNAME | SHGFI_TYPENAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) {
            it.label = sfi.szDisplayName;
            it.type = sfi.szTypeName;
            it.icon = sfi.iIcon;
        }
        ULARGE_INTEGER freeBytes{}, total{};
        if (GetDiskFreeSpaceExW(it.name.c_str(), nullptr, &total, &freeBytes)) {
            wchar_t f[32], t[32];
            StrFormatByteSizeW(static_cast<LONGLONG>(freeBytes.QuadPart), f, 32);
            StrFormatByteSizeW(static_cast<LONGLONG>(total.QuadPart), t, 32);
            it.type += std::wstring(L"  \u00b7  ") + f + L" free of " + t;
        }
        r.items.push_back(std::move(it));
    }
}

}  // namespace

Listing listFolder(const std::wstring& path, bool showHidden) {
    Listing r;
    r.path = path;
    if (path.empty()) {
        listDrives(r);
        return r;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(joinPath(path, L"*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        r.error = GetLastError();
        // An empty drive root has no "." entry, so "not found" just means empty.
        if (r.error == ERROR_FILE_NOT_FOUND && isDirectory(path)) r.error = 0;
        DWORD attrs = r.error ? GetFileAttributesW(path.c_str()) : INVALID_FILE_ATTRIBUTES;
        r.isFile = attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
        return r;
    }
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (!showHidden && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        Item it;
        it.name = n;
        it.attrs = fd.dwFileAttributes;
        it.modified = fd.ftLastWriteTime;
        it.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        r.items.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return r;
}

void sortItems(std::vector<Item>& items, SortKey key, bool ascending) {
    std::stable_sort(items.begin(), items.end(), [=](const Item& a, const Item& b) {
        if (a.isDir() != b.isDir()) return a.isDir();
        int c = 0;
        switch (key) {
        case SortKey::Modified: c = CompareFileTime(&a.modified, &b.modified); break;
        case SortKey::Size: c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
        case SortKey::Type:   // by the type name shown in the list when known, else by extension
            c = a.type.empty() || b.type.empty() ? CompareStringOrdinal(extPtr(a.name), -1, extPtr(b.name), -1, TRUE) - CSTR_EQUAL
                                                 : StrCmpLogicalW(a.type.c_str(), b.type.c_str());
            break;
        case SortKey::Name: break;
        }
        if (c == 0) c = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
        return ascending ? c < 0 : c > 0;
    });
}

bool matchesFilter(const Item& item, const std::wstring& filter) {
    if (filter.empty()) return true;
    if (filter.find_first_of(L"*?") != std::wstring::npos)
        return PathMatchSpecW(item.shown().c_str(), filter.c_str()) != FALSE;
    return StrStrIW(item.shown().c_str(), filter.c_str()) != nullptr;
}

std::wstring normalizePath(std::wstring p) {
    auto junk = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'"'; };
    while (!p.empty() && junk(p.back())) p.pop_back();
    size_t start = 0;
    while (start < p.size() && junk(p[start])) ++start;
    p.erase(0, start);
    if (p.empty() || equalNoCase(p.c_str(), -1, L"This PC", -1)) return L"";

    if (p.find(L'%') != std::wstring::npos) {
        std::wstring buf(32768, L'\0');
        DWORD n = ExpandEnvironmentStringsW(p.c_str(), buf.data(), DWORD(buf.size()));
        if (n > 0 && n <= buf.size()) p.assign(buf.c_str());
    }
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    if (isUnc(p) || (p.size() >= 3 && p[1] == L':' && p[2] == L'\\')) {
        std::wstring buf(32768, L'\0');
        DWORD n = GetFullPathNameW(p.c_str(), DWORD(buf.size()), buf.data(), nullptr);
        if (n > 0 && n < buf.size()) p.assign(buf.c_str());   // resolves "." and ".."
    }
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() >= 2 && p[1] == L':') p[0] = towupper(p[0]);
    return p;
}

std::wstring resolveArgument(const std::wstring& arg) {
    std::wstring full(32768, L'\0');
    DWORD n = GetFullPathNameW(arg.c_str(), DWORD(full.size()), full.data(), nullptr);
    return normalizePath(n > 0 && n < full.size() ? full.substr(0, n) : arg);
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (isSep(dir.back())) return dir + name;
    return dir + L'\\' + name;
}

std::wstring parentPath(const std::wstring& path) {
    if (path.empty() || isDriveRoot(path)) return L"";
    size_t pos = path.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"";
    if (isUnc(path)) {
        // "\\server\share" is the top of a network path.
        size_t shareSep = path.find(L'\\', 2);
        if (shareSep == std::wstring::npos || pos <= shareSep) return L"";
        return path.substr(0, pos);
    }
    std::wstring parent = path.substr(0, pos);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    return parent;
}

std::wstring leafName(const std::wstring& path) {
    if (path.empty() || isDriveRoot(path)) return path;
    size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring displayName(const std::wstring& path) {
    return path.empty() ? L"This PC" : leafName(path);
}

std::wstring childToward(const std::wstring& ancestor, const std::wstring& descendant) {
    if (ancestor.empty()) {
        if (descendant.size() >= 3 && descendant[1] == L':') return descendant.substr(0, 3);
        return L"";
    }
    std::wstring prefix = ancestor;
    if (!isSep(prefix.back())) prefix += L'\\';
    int n = int(prefix.size());
    if (descendant.size() <= prefix.size() || !equalNoCase(descendant.c_str(), n, prefix.c_str(), n))
        return L"";
    size_t end = descendant.find(L'\\', prefix.size());
    return descendant.substr(prefix.size(), end == std::wstring::npos ? std::wstring::npos : end - prefix.size());
}

bool samePath(const std::wstring& a, const std::wstring& b) {
    return equalNoCase(a.c_str(), int(a.size()), b.c_str(), int(b.size()));
}

bool isDirectory(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isAbsolutePath(const std::wstring& path) {
    return (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') || isUnc(path);
}

std::wstring toLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

std::wstring extensionOf(const std::wstring& name) {
    return toLower(extPtr(name));
}

std::wstring formatSize(uint64_t bytes) {
    wchar_t buf[64];
    return StrFormatKBSizeW(static_cast<LONGLONG>(bytes), buf, 64) ? buf : L"";
}

std::wstring formatTime(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return L"";
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return L"";
    wchar_t d[64] = {}, t[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, t, 64);
    return std::wstring(d) + L"  " + t;
}

std::wstring errorText(DWORD error) {
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, buf, 512, nullptr);
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s.empty() ? L"Error " + std::to_wstring(error) : s;
}
