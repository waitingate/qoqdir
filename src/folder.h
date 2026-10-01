#pragma once
// Folder contents and path rules. No UI here, so all of it is unit-tested.
//
// Paths are plain Win32 paths ("C:\Users", "\\server\share\dir").
// The empty path "" means "This PC", the list of drives.

#include "common.h"

#include <cstdint>

struct Item {
    std::wstring name;    // file name, or "C:\" for a drive
    std::wstring label;   // text to show instead of name (drives only)
    std::wstring type;    // type text to show; filled in by the file list (sorting by Type uses it)
    uint64_t size = 0;
    FILETIME modified{};
    DWORD attrs = 0;
    int icon = -1;        // system image list index; -1 until first shown

    bool isDir() const { return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    const std::wstring& shown() const { return label.empty() ? name : label; }
};

struct Listing {
    std::wstring path;
    std::vector<Item> items;
    DWORD error = 0;      // Win32 error code, 0 on success
    bool isFile = false;  // with an error: the path exists but is a file
};

// Reads a folder. Blocking: call it off the UI thread.
Listing listFolder(const std::wstring& path, bool showHidden);

enum class SortKey { Name = 0, Modified = 1, Type = 2, Size = 3 };
// Folders always come first; names compare the way Explorer does ("file2" < "file10").
void sortItems(std::vector<Item>& items, SortKey key, bool ascending);

// Case-insensitive substring match, or a wildcard match when the filter contains * or ?.
bool matchesFilter(const Item& item, const std::wstring& filter);

std::wstring normalizePath(std::wstring text);       // cleans up typed text; "" for This PC
std::wstring resolveArgument(const std::wstring& arg); // a command-line folder, relative to the current one
// Adds folders given on the command line to the saved tabs (each folder once) and returns
// the active tab: the first given folder, or `active` if none were given.
int addStartFolders(std::vector<std::wstring>& tabs, int active, const std::vector<std::wstring>& given);

// The command line, 7z style: one-letter switches with the value attached.
//   qoqdir [switches] [folders...]     (see kUsage in main.cpp)
struct CommandLine {
    std::vector<std::wstring> folders;   // -t{folder} and plain folders, in order
    bool onlyThese = false;              // -n: only these tabs; the saved tabs are left alone
    bool help = false;                   // -h or -?
    bool selfTest = false;               // --selftest
    std::wstring error;                  // a mistake to show with the help
};
CommandLine parseCommandLine(const std::vector<std::wstring>& args);
std::wstring joinPath(const std::wstring& dir, const std::wstring& name);
std::wstring parentPath(const std::wstring& path);   // "" when the parent is This PC
std::wstring leafName(const std::wstring& path);
std::wstring displayName(const std::wstring& path);  // leaf name, or "This PC"
std::wstring locationText(const std::wstring& path); // for the address bar: the path, or "This PC"
// Name of the child of 'ancestor' that leads to 'descendant', or "" if it is not inside.
std::wstring childToward(const std::wstring& ancestor, const std::wstring& descendant);
bool samePath(const std::wstring& a, const std::wstring& b);
bool isDirectory(const std::wstring& path);
bool isAbsolutePath(const std::wstring& path);   // "C:\..." or "\\server\...", not a command like "cmd"

std::wstring toLower(std::wstring s);
std::wstring extensionOf(const std::wstring& name);  // lower case with the dot, "" if none
std::wstring formatSize(uint64_t bytes);
std::wstring formatTime(const FILETIME& ft);
std::wstring errorText(DWORD error);
