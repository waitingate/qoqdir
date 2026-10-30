#pragma once
// Folder contents and path rules, with no UI, so all of it is unit-tested. Paths are plain Win32 paths
// ("C:\Users", "\\server\share\dir"); "" is This PC, the list of drives.

#include "util.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <shellapi.h>

// ---------------------------------------------------------------- paths

std::wstring joinPath(const std::wstring& dir, const std::wstring& name);
std::wstring parentPath(const std::wstring& path);   // "" when the parent is This PC
std::wstring leafName(const std::wstring& path);
std::wstring displayName(const std::wstring& path);  // leaf name, or "This PC"
std::wstring locationText(const std::wstring& path); // for the address bar: the path, or "This PC"
// Name of the child of 'ancestor' that leads to 'descendant', or "" if it is not inside.
std::wstring childToward(const std::wstring& ancestor, const std::wstring& descendant);
bool samePath(const std::wstring& a, const std::wstring& b);
bool isDirectory(const std::wstring& path);
bool exists(const std::wstring& path);           // a file or a folder is there
bool isAbsolutePath(const std::wstring& path);   // "C:\..." or "\\server\...", not a command like "cmd"
bool isRelativePath(const std::wstring& text);   // ".", "..\x", "sub\deeper": a place here, not a command
// Cleans up a path; "" for This PC. Leaves %VARS% alone: a folder may really be named "%TEMP%".
std::wstring normalizePath(std::wstring text);

// ---------------------------------------------------------------- typed text and the command line

std::wstring expandVars(const std::wstring& text);     // only for what the user typed (5.3)
std::wstring resolveArgument(const std::wstring& arg); // a command-line folder, relative to the current one
std::wstring parentProcessName();   // the program this one was started from ("powershell.exe"), lower case; "" if unknown

// The command line: qoqdir [switches] [folders...] (see 5.10 in docs/SPEC.md).
struct CommandLine {
    std::vector<std::wstring> folders;   // -t= / --tab= folders and plain folders, in order, each once
    bool help = false;                   // -h or -?
    bool selfTest = false;               // --selftest
    std::wstring error;                  // a mistake to show with the help
};
CommandLine parseCommandLine(const std::vector<std::wstring>& args);
// "--tab=" and the folder, for a Qoqdir started on it (Open in new window, 5.8), quoted so that the command-line rules
// give it back whole.
std::wstring tabArgument(const std::wstring& folder);
std::vector<std::wstring> commandLineWords(const std::wstring& line);   // as Windows splits it (CommandLineToArgvW)

// ---------------------------------------------------------------- reading a folder

inline const std::wstring kNoTypeName;   // Item::typeText's ""; a local static would need a guard (winpthreads)

struct Item {
    std::wstring name;    // file name, or "C:\" for a drive
    std::wstring label;   // text to show instead of name (drives only)
    // The type name shown, from the shell: the loader fills it (fillTypes), the list what is left. Shared by the items
    // of one type: a copy in each cost 140 bytes an item, 1.4 MB for 10,000. Null while unknown; the shell can name
    // a type "", and that is known.
    std::shared_ptr<const std::wstring> type;
    uint64_t size = 0;
    FILETIME modified{};
    DWORD attrs = 0;
    int icon = -1;        // system image list index; -1 until first shown

    bool isDir() const { return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    const std::wstring& shown() const { return label.empty() ? name : label; }
    const std::wstring& typeText() const { return type ? *type : kNoTypeName; }   // "" while unknown too
};

enum class SortKey { Name = 0, Modified = 1, Type = 2, Size = 3 };

struct Listing {
    std::wstring path;
    std::vector<Item> items;
    DWORD error = 0;      // Win32 error code, 0 on success
    bool isFile = false;  // with an error: the path exists but is a file
    FILETIME written{};   // the folder's own last write, as it was before the read (folderWritten)
    std::wstring file{};  // the file a tab's read found in the folder's place, in this folder (listTabFolder)
    std::optional<std::pair<SortKey, bool>> sorted{};   // how the items are sorted, if they are: key, ascending
};

// Reads a folder; blocking, so off the UI thread. Once `stop` says true it ends with ERROR_OPERATION_ABORTED.
Listing listFolder(const std::wstring& path, bool showHidden, const std::function<bool()>& stop = nullptr);
// The same for a tab's own folder, which may be a file (qoqdir C:\src\main.cpp): then the folder it is in, with `file`
// naming it, as explorer /select shows it (5.10).
Listing listTabFolder(const std::wstring& path, bool showHidden, const std::function<bool()>& stop = nullptr);
std::vector<std::wstring> driveRoots();   // "C:\" and so on, for the drive letters in use
FILETIME folderWritten(const std::wstring& path);   // the folder's own last-write time (zero if unknown): it changes with its contents

// ---------------------------------------------------------------- sorting and filtering

// Folders always come first; names compare the way Explorer does ("file2" < "file10").
void sortItems(std::vector<Item>& items, SortKey key, bool ascending);

// A filter as typed, made ready for many items: a case-insensitive substring, or a wildcard match when it contains * or
// ?, in one pattern or several separated by ';' ("*.jpg; *.png").
class ItemFilter {
public:
    explicit ItemFilter(const std::wstring& text);
    bool matches(const Item& item);

private:
    std::wstring text_;
    std::vector<std::wstring> patterns_;   // in lower case; none for a plain substring
    std::wstring name_;                    // the item's name in lower case: its room is kept from item to item
};

// After deleting 'rows' (ascending), the row of the item that takes the first one's place, else the one before.
int rowAfterDeleting(const std::vector<int>& rows, int count);

// ---------------------------------------------------------------- text

// What the shell says about a path (or an extension probe with SHGFI_USEFILEATTRIBUTES): none when it fails.
std::optional<SHFILEINFOW> fileInfo(const std::wstring& path, DWORD attrs, UINT flags);
std::string toUtf8(const std::wstring& text);
std::wstring toLower(std::wstring s);
std::wstring extensionOf(const std::wstring& name);  // lower case with the dot, "" if none
bool isShortcut(const std::wstring& name);           // .lnk or .url
std::wstring formatSize(uint64_t bytes);   // "1,234 KB", as in Explorer's Size column
std::wstring byteSize(uint64_t bytes);     // "1.20 MB"
std::wstring formatTime(const FILETIME& ft);
std::wstring errorText(DWORD error);
// An ini file's section as GetPrivateProfileSectionW gives it, "key=value" each ending in a zero: keys in lower case,
// as the file's are matched, and of a key there twice the first. Spaces round both, and quotes round a value, go, as
// GetPrivateProfileStringW drops them.
std::map<std::wstring, std::wstring> parseIniSection(std::wstring_view text);

// ---------------------------------------------------------------- drops

// The keys a folder's drop target is told (5.8): a plain drag that could move files goes on as a right-button drag, so
// Windows asks with its menu (Copy here, Move here...) instead of moving them at once. Ctrl, Shift or Alt held says
// what to do, and a drag that can only copy has nothing to ask.
DWORD dropKeys(DWORD keys, DWORD allowed);
// Why a drop into 'dir' would leave the 'dragged' files where they are (4.8): 'dir' is the folder one of them is in
// (source), or one of them or a folder inside one (itself: a folder can't go into itself). None when it is neither.
enum class DropBack { source, itself };
std::optional<DropBack> dropBack(const std::wstring& dir, const std::vector<std::wstring>& dragged);
