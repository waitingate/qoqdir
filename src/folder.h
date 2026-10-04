#pragma once
// Folder contents and path rules. No UI here, so all of it is unit-tested.
//
// Paths are plain Win32 paths ("C:\Users", "\\server\share\dir").
// The empty path "" means "This PC", the list of drives.

#include "common.h"

#include <cstdint>
#include <functional>

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

// Reads a folder. Blocking: call it off the UI thread. `stop` is asked every few hundred entries; once it
// says true the read ends at once with ERROR_OPERATION_ABORTED and no items.
Listing listFolder(const std::wstring& path, bool showHidden, const std::function<bool()>& stop = nullptr);

enum class SortKey { Name = 0, Modified = 1, Type = 2, Size = 3 };
// Folders always come first; names compare the way Explorer does ("file2" < "file10").
void sortItems(std::vector<Item>& items, SortKey key, bool ascending);

// Case-insensitive substring match, or a wildcard match when the filter contains * or ?.
bool matchesFilter(const Item& item, const std::wstring& filter);

// Cleans up a path; "" for This PC. Leaves %VARS% alone: a folder may really be named "%TEMP%".
std::wstring normalizePath(std::wstring text);
std::wstring expandVars(const std::wstring& text);     // only for what the user typed (5.3)
std::wstring resolveArgument(const std::wstring& arg); // a command-line folder, relative to the current one

// The command line, 7z style: one-letter switches with the value attached.
//   qoqdir [switches] [folders...]     (see kUsage in main.cpp)
struct CommandLine {
    std::vector<std::wstring> folders;   // -t{folder} and plain folders, in order, each once
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
bool isRelativePath(const std::wstring& text);   // ".", "..\x", "sub\deeper": a place here, not a command

// The row to select after deleting 'rows' (ascending): the item that takes the first deleted one's place,
// else the one before it; -1 when nothing is left. (4.6: "the item that followed the deleted ones")
int rowAfterDeleting(const std::vector<int>& rows, int count);

std::wstring toLower(std::wstring s);
std::wstring extensionOf(const std::wstring& name);  // lower case with the dot, "" if none
bool isShortcut(const std::wstring& name);           // .lnk or .url
std::wstring formatSize(uint64_t bytes);   // "1,234 KB", as in Explorer's Size column
std::wstring byteSize(uint64_t bytes);     // "1.20 MB"
std::wstring formatTime(const FILETIME& ft);
std::wstring errorText(DWORD error);
