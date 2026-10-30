// Paths, folder reading, sorting and filtering (folder.h): the part with no UI, which the unit tests cover.
#include "folder.h"

#include <tlhelp32.h>

#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace {

bool isSep(wchar_t c) { return c == L'\\' || c == L'/'; }
bool isDriveRoot(const std::wstring& p) { return p.size() == 3 && p[1] == L':' && p[2] == L'\\'; }
bool isUnc(const std::wstring& p) { return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\'; }

bool equalNoCase(const wchar_t* a, int an, const wchar_t* b, int bn) {
    return CompareStringOrdinal(a, an, b, bn, TRUE) == CSTR_EQUAL;
}

// Relative to the current folder, with "." and ".." resolved; the path itself if Windows cannot.
std::wstring fullPath(const std::wstring& p) {
    // Most paths fit on the stack; a longer one gets the size Windows asks for (with its terminating zero), not 64 KB
    // for every path, which a drag of 10,000 files had to fill one by one.
    wchar_t small[MAX_PATH];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, small, nullptr);
    if (n > 0 && n < MAX_PATH) return std::wstring(small, n);
    if (n == 0) return p;
    std::wstring buf(n, L'\0');
    DWORD m = GetFullPathNameW(p.c_str(), n, buf.data(), nullptr);
    return m > 0 && m < n ? buf.substr(0, m) : p;
}

// Pointer to the extension inside name (".txt"), or to the terminating zero.
const wchar_t* extPtr(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name.c_str() + name.size() : name.c_str() + dot;
}

}  // namespace

// ---------------------------------------------------------------- paths

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    // A name from the root ("\Windows", as typed): on the folder's own drive or share, as Explorer takes it and as
    // the Rust build's Path::join does.
    if (!name.empty() && isSep(name[0]) && !isUnc(name)) {
        std::wstring root = dir;
        while (!parentPath(root).empty()) root = parentPath(root);   // "C:\" or "\\server\share"
        if (isSep(root.back())) root.pop_back();
        return root + name;
    }
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

std::wstring locationText(const std::wstring& path) {
    return path.empty() ? L"This PC" : path;
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

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool isAbsolutePath(const std::wstring& path) {
    return (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') || isUnc(path);
}

bool isRelativePath(const std::wstring& text) {
    if (text.empty() || isAbsolutePath(text)) return false;
    if (text[0] == L'.') return true;   // ".", ".\sub", "..\other", "..\"
    // Only the first word counts ("notepad a\b.txt" stays a command); a colon marks a web address ("https:\\x").
    size_t end = text.find_first_of(L" \t");   // npos when the text is one word
    return text.find(L'\\') < end && text.find(L':') >= end;
}

std::wstring normalizePath(std::wstring p) {
    auto junk = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'"'; };
    while (!p.empty() && junk(p.back())) p.pop_back();
    size_t start = 0;
    while (start < p.size() && junk(p[start])) ++start;
    p.erase(0, start);
    if (p.empty() || equalNoCase(p.c_str(), -1, L"This PC", -1)) return L"";
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    if (isAbsolutePath(p)) p = fullPath(p);   // resolves "." and ".."
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() >= 2 && p[1] == L':') p[0] = towupper(p[0]);
    return p;
}

// ---------------------------------------------------------------- typed text and the command line

std::wstring expandVars(const std::wstring& text) {
    std::wstring buf(32768, L'\0');
    DWORD n = ExpandEnvironmentStringsW(text.c_str(), buf.data(), DWORD(buf.size()));
    return n > 0 && n <= buf.size() ? std::wstring(buf.c_str()) : text;
}

std::wstring resolveArgument(const std::wstring& arg) {
    // Clean it first (quotes, spaces, "D:" -> "D:\", as in Explorer), then resolve against the working folder only
    // what is still relative, so a quoted "D:\" from cmd (it arrives as D:") is not taken as D:'s current folder.
    std::wstring a = normalizePath(arg);
    return a.empty() || isAbsolutePath(a) ? a : normalizePath(fullPath(a));
}

std::wstring parentProcessName() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    DWORD me = GetCurrentProcessId(), parent = 0;
    std::wstring name;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == me) parent = e.th32ParentProcessID;
    for (BOOL ok = parent && Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == parent) name = toLower(e.szExeFile);
    CloseHandle(snap);
    return name;
}

CommandLine parseCommandLine(const std::vector<std::wstring>& args) {
    CommandLine cmd;
    auto addFolder = [&](const std::wstring& text) {
        std::wstring folder = resolveArgument(text);
        bool known = std::any_of(cmd.folders.begin(), cmd.folders.end(), [&](auto& f) { return samePath(f, folder); });
        if (!folder.empty() && !known) cmd.folders.push_back(folder);   // each folder once
    };
    for (const std::wstring& arg : args) {
        if (arg.empty() || arg[0] != L'-') addFolder(arg);   // a plain folder
        else if (arg == L"--selftest") cmd.selfTest = true;
        else if (arg == L"-h" || arg == L"-?" || arg == L"--help") cmd.help = true;
        // All after "=" is the folder, even a name that starts with "-". "-t=" is short for "--tab=", but Windows
        // PowerShell takes "-t=" for a switch of its own: unquoted, it splits "-t=my.folder" at the dot and keeps
        // "-t=$HOME" as typed, so there the folder after "-t=" needs quotes (5.10).
        else if (arg.starts_with(L"--tab=") || arg.starts_with(L"-t=")) addFolder(arg.substr(arg.find(L'=') + 1));
        else cmd.error = L"Unknown option: " + arg;
        if (!cmd.error.empty()) break;
    }
    return cmd;
}

std::wstring tabArgument(const std::wstring& folder) {
    // Backslashes before a quote escape it (CommandLineToArgvW and Rust's std read them alike), so the ones the
    // folder ends in are doubled: "D:\\" comes back as D:\, where "D:\" would end in a quote.
    std::wstring arg = L"--tab=\"" + folder;
    for (size_t i = folder.size(); i > 0 && folder[i - 1] == L'\\'; --i) arg += L'\\';
    return arg + L'"';
}

std::vector<std::wstring> commandLineWords(const std::wstring& line) {
    std::vector<std::wstring> words;
    int count = 0;
    if (LPWSTR* list = CommandLineToArgvW(line.c_str(), &count)) {
        words.assign(list, list + count);
        LocalFree(list);
    }
    return words;
}

// ---------------------------------------------------------------- reading a folder

std::vector<std::wstring> driveRoots() {
    std::vector<std::wstring> roots;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) roots.push_back(std::wstring(1, wchar_t(L'A' + i)) + L":\\");
    return roots;
}

namespace {
struct FindCloser {
    void operator()(HANDLE h) const { FindClose(h); }
};

void listDrives(Listing& r) {
    for (std::wstring& root : driveRoots()) {
        Item it;
        it.name = std::move(root);
        it.attrs = FILE_ATTRIBUTE_DIRECTORY;
        std::wstring type;
        if (auto sfi = fileInfo(it.name, 0, SHGFI_DISPLAYNAME | SHGFI_TYPENAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) {
            it.label = sfi->szDisplayName;
            type = sfi->szTypeName;
            it.icon = sfi->iIcon;
        }
        ULARGE_INTEGER freeBytes{}, total{};
        // Free bytes available to the caller (respecting a quota), as Explorer shows and the Rust version reads.
        if (GetDiskFreeSpaceExW(it.name.c_str(), &freeBytes, &total, nullptr))
            type += L"  \u00b7  " + byteSize(freeBytes.QuadPart) + L" free of " + byteSize(total.QuadPart);
        if (!type.empty()) it.type = std::make_shared<const std::wstring>(std::move(type));
        r.items.push_back(std::move(it));
    }
}
}  // namespace

Listing listFolder(const std::wstring& path, bool showHidden, const std::function<bool()>& stop) {
    Listing r;
    r.path = path;
    if (path.empty()) {
        listDrives(r);
        return r;
    }
    r.written = folderWritten(path);   // before the read: a change during it then shows as a newer time
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
    std::unique_ptr<std::remove_pointer_t<HANDLE>, FindCloser> owned(h);   // closed on every return below
    size_t seen = 0;
    do {
        if (stop && ++seen % 256 == 0 && stop()) return Listing{path, {}, ERROR_OPERATION_ABORTED};
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
    DWORD end = GetLastError();   // ERROR_NO_MORE_FILES, or why the read broke off (e.g. the network went)
    if (end != ERROR_NO_MORE_FILES) return Listing{path, {}, end};   // never half a folder as all of it
    // Grown by doubling, the vector has room for up to twice its items, kept while the folder is shown: 0.7 MB for
    // 10,000. Moving them to a block their size takes well under a millisecond.
    r.items.shrink_to_fit();
    return r;
}

FILETIME folderWritten(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);   // leaves the zeros if it fails
    return data.ftLastWriteTime;
}

// ---------------------------------------------------------------- sorting and filtering

namespace {
// A string's sort key: comparing two keys byte by byte orders them exactly as StrCmpLogicalW (Explorer) does,
// checked on every pair of 1,500 tricky names, without asking Windows again for each of the ~130,000
// comparisons a 10,000-file sort makes.
std::string sortKey(const std::wstring& s) {
    constexpr DWORD flags = LCMAP_SORTKEY | NORM_IGNORECASE | SORT_DIGITSASNUMBERS;
    auto map = [&](void* to, int bytes) {   // with LCMAP_SORTKEY the sizes count bytes
        return LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, s.c_str(), int(s.size()), static_cast<LPWSTR>(to), bytes,
                             nullptr, nullptr, 0);
    };
    // First into a buffer on the stack, which holds most names' keys: asking Windows for the size first costs as much
    // as the key itself.
    wchar_t small[256];
    if (int n = map(small, int(sizeof small)); n > 0)
        return std::string(reinterpret_cast<const char*>(small), size_t(n));
    int n = std::max(map(nullptr, 0), 0);
    std::string key(size_t(n), '\0');
    if (n > 0) map(key.data(), n);
    return key;
}
}  // namespace

void sortItems(std::vector<Item>& items, SortKey key, bool ascending) {
    // Sorts the items' numbers, then moves each item once. Sort keys are made on first use and kept: a date or
    // size sort needs the names only for ties.
    std::vector<std::string> names(items.size());
    auto keyOf = [](std::vector<std::string>& keys, size_t i, const std::wstring& text) -> const std::string& {
        if (keys[i].empty()) keys[i] = sortKey(text);
        return keys[i];
    };
    // The type names' keys, one per type (a folder holds few), and the one each item has.
    std::vector<std::string> typeKeys;
    std::vector<size_t> typeOf;
    if (key == SortKey::Type) {
        std::unordered_map<std::wstring, size_t> seen;
        for (const Item& it : items) {
            auto [at, fresh] = seen.try_emplace(it.typeText(), typeKeys.size());
            if (fresh) typeKeys.push_back(sortKey(it.typeText()));
            typeOf.push_back(at->second);
        }
    }
    std::vector<size_t> order(items.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t i, size_t j) {
        const Item &a = items[i], &b = items[j];
        if (a.isDir() != b.isDir()) return a.isDir();
        int c = 0;
        switch (key) {
        case SortKey::Modified: c = CompareFileTime(&a.modified, &b.modified); break;
        case SortKey::Size: c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
        case SortKey::Type:   // by the type name shown in the list when known, else by extension
            if (!a.type || !b.type)
                c = CompareStringOrdinal(extPtr(a.name), -1, extPtr(b.name), -1, TRUE) - CSTR_EQUAL;
            else if (typeOf[i] != typeOf[j])   // the same type needs no comparing
                c = typeKeys[typeOf[i]].compare(typeKeys[typeOf[j]]);
            break;
        case SortKey::Name: break;
        }
        if (c == 0) c = keyOf(names, i, a.name).compare(keyOf(names, j, b.name));
        return ascending ? c < 0 : c > 0;
    });
    std::vector<Item> sorted;
    sorted.reserve(items.size());
    for (size_t i : order) sorted.push_back(std::move(items[i]));
    items = std::move(sorted);
}

namespace {
// '*' is any run of characters and '?' any one, both strings in lower case. On a mismatch only the last star takes
// one more character, so a name takes at most (name x pattern) steps. Windows' PathMatchSpecW tries every way of
// splitting the name between the stars (8.8 s for "*a*a*a*a*a*a*a*b" on a 40-letter name), and under a double-byte
// code page it misreads Chinese letters.
bool wildcardMatch(std::wstring_view name, std::wstring_view pattern) {
    size_t n = 0, p = 0, star = pattern.npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || pattern[p] == name[n])) { ++n; ++p; }
        else if (p < pattern.size() && pattern[p] == L'*') { star = p++; mark = n; }
        else if (star != pattern.npos) { p = star + 1; n = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}
}  // namespace

bool matchesFilter(const Item& item, const std::wstring& filter) {
    if (filter.empty()) return true;
    if (filter.find_first_of(L"*?") == std::wstring::npos)
        return StrStrIW(item.shown().c_str(), filter.c_str()) != nullptr;
    // Wildcards, ignoring case, in one pattern or several separated by ';' ("*.jpg; *.png").
    std::wstring name = toLower(item.shown()), patterns = toLower(filter) + L';';
    for (size_t at = 0, end; (end = patterns.find(L';', at)) != std::wstring::npos; at = end + 1) {
        size_t start = patterns.find_first_not_of(L' ', at);   // never past the ';'
        if (wildcardMatch(name, std::wstring_view(patterns).substr(start, end - start))) return true;
    }
    return false;
}

int rowAfterDeleting(const std::vector<int>& rows, int count) {
    if (rows.empty()) return -1;
    int next = rows.front();
    for (int row : rows) {   // past the block of selected rows that starts at the first one
        if (row != next) break;
        ++next;
    }
    return next < count ? next : rows.front() - 1;   // nothing after them: the one before, or -1
}

// ---------------------------------------------------------------- text

std::optional<SHFILEINFOW> fileInfo(const std::wstring& path, DWORD attrs, UINT flags) {
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(path.c_str(), attrs, &sfi, sizeof sfi, flags)) return std::nullopt;
    return sfi;
}

std::string toUtf8(const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size_t(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), utf8.data(), n, nullptr, nullptr);
    return utf8;
}

std::wstring toLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

std::wstring extensionOf(const std::wstring& name) {
    return toLower(extPtr(name));
}

bool isShortcut(const std::wstring& name) {
    std::wstring ext = extensionOf(name);
    return ext == L".lnk" || ext == L".url";
}

std::wstring formatSize(uint64_t bytes) {
    wchar_t buf[64];
    return StrFormatKBSizeW(static_cast<LONGLONG>(bytes), buf, 64) ? buf : L"";
}

std::wstring byteSize(uint64_t bytes) {
    wchar_t buf[64];
    return StrFormatByteSizeW(static_cast<LONGLONG>(bytes), buf, 64) ? buf : L"";
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

std::map<std::wstring, std::wstring> parseIniSection(std::wstring_view text) {
    auto trim = [](std::wstring_view s) {
        size_t from = s.find_first_not_of(L" \t");
        return from == s.npos ? std::wstring_view() : s.substr(from, s.find_last_not_of(L" \t") - from + 1);
    };
    std::map<std::wstring, std::wstring> values;
    for (size_t at = 0; at < text.size();) {
        size_t end = std::min(text.find(L'\0', at), text.size());
        std::wstring_view line = text.substr(at, end - at);
        at = end + 1;
        size_t eq = line.find(L'=');
        if (eq == line.npos) continue;
        std::wstring_view value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value[0] == L'"' || value[0] == L'\'') && value.back() == value[0])
            value = value.substr(1, value.size() - 2);
        values.emplace(toLower(std::wstring(trim(line.substr(0, eq)))), value);
    }
    return values;
}

// ---------------------------------------------------------------- drops

DWORD dropKeys(DWORD keys, DWORD allowed) {
    DWORD deliberate = MK_RBUTTON | MK_SHIFT | MK_CONTROL | MK_ALT;   // a key or button that says what to do
    bool plain = (keys & MK_LBUTTON) && !(keys & deliberate);
    return plain && (allowed & DROPEFFECT_MOVE) ? (keys & ~DWORD(MK_LBUTTON)) | MK_RBUTTON : keys;
}

std::optional<DropBack> dropBack(const std::wstring& dir, const std::vector<std::wstring>& dragged) {
    for (const std::wstring& path : dragged) {
        if (path.empty()) continue;   // This PC carries nothing a folder holds
        if (samePath(dir, parentPath(path))) return DropBack::source;
        if (samePath(dir, path) || !childToward(path, dir).empty()) return DropBack::itself;
    }
    return std::nullopt;
}
