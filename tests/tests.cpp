// Tests for the path and folder rules. Run: ctest --test-dir build  (or build\unit_tests.exe)
#include "folder.h"

#include <cstdio>

static int failures = 0;

#define CHECK_EQ(actual, expected)                                                               \
    do {                                                                                         \
        auto a_ = (actual);                                                                      \
        auto e_ = (expected);                                                                    \
        if (!(a_ == e_)) {                                                                       \
            ++failures;                                                                          \
            fwprintf(stderr, L"FAIL line %d: %hs\n  got      [%ls]\n  expected [%ls]\n", __LINE__, \
                     #actual, std::wstring(a_).c_str(), std::wstring(e_).c_str());               \
        }                                                                                        \
    } while (0)

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++failures;                                                 \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);     \
        }                                                               \
    } while (0)

static Item file(const wchar_t* name, uint64_t size = 0, bool dir = false) {
    Item it;
    it.name = name;
    it.size = size;
    it.attrs = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    return it;
}

static Item dated(const wchar_t* name, uint64_t time) {   // a file with a last-write time
    Item it = file(name);
    it.modified = {DWORD(time), DWORD(time >> 32)};
    return it;
}

static std::wstring names(const std::vector<Item>& items) {
    std::wstring s;
    for (const auto& it : items) s += (s.empty() ? L"" : L",") + it.name;
    return s;
}

int main() {
    // parentPath
    CHECK_EQ(parentPath(L"C:\\Users\\me"), L"C:\\Users");
    CHECK_EQ(parentPath(L"C:\\Users"), L"C:\\");
    CHECK_EQ(parentPath(L"C:\\"), L"");
    CHECK_EQ(parentPath(L""), L"");
    CHECK_EQ(parentPath(L"\\\\server\\share\\dir"), L"\\\\server\\share");
    CHECK_EQ(parentPath(L"\\\\server\\share"), L"");
    CHECK_EQ(parentPath(L"\\\\server"), L"");

    // joinPath / leafName / displayName
    CHECK_EQ(joinPath(L"C:\\", L"a"), L"C:\\a");
    CHECK_EQ(joinPath(L"C:\\a", L"b"), L"C:\\a\\b");
    CHECK_EQ(joinPath(L"", L"D:\\"), L"D:\\");
    CHECK_EQ(leafName(L"C:\\a\\b"), L"b");
    CHECK_EQ(leafName(L"C:\\"), L"C:\\");
    CHECK_EQ(displayName(L""), L"This PC");

    // normalizePath
    CHECK_EQ(normalizePath(L"  \"c:/Windows/\"  "), L"C:\\Windows");
    CHECK_EQ(normalizePath(L"d:"), L"D:\\");
    CHECK_EQ(normalizePath(L"C:\\a\\..\\b"), L"C:\\b");
    CHECK_EQ(normalizePath(L"This PC"), L"");
    CHECK_EQ(normalizePath(L"   "), L"");
    {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        CHECK_EQ(resolveArgument(L"."), normalizePath(cwd));   // "qoqdir ." from a terminal
        CHECK_EQ(resolveArgument(L"C:\\Windows\\"), L"C:\\Windows");
        CHECK_EQ(resolveArgument(L"C:"), L"C:\\");   // a drive alone is its root, as in Explorer
    }
    CHECK_EQ(normalizePath(L"\\\\server\\share\\"), L"\\\\server\\share");
    {
        wchar_t windir[MAX_PATH];
        GetEnvironmentVariableW(L"SystemRoot", windir, MAX_PATH);
        CHECK(samePath(normalizePath(L"%SystemRoot%"), windir));
    }
    CHECK_EQ(normalizePath(L"C:\\a\\%TEMP%\\.\\sub", false), L"C:\\a\\%TEMP%\\sub");   // a folder really named %TEMP%

    // childToward: selects the folder you came from after going up
    CHECK_EQ(childToward(L"C:\\Users", L"C:\\Users\\me\\Documents"), L"me");
    CHECK_EQ(childToward(L"C:\\", L"C:\\Users"), L"Users");
    CHECK_EQ(childToward(L"c:\\users", L"C:\\Users\\me"), L"me");
    CHECK_EQ(childToward(L"", L"D:\\Photos"), L"D:\\");
    CHECK_EQ(childToward(L"C:\\Users", L"C:\\Users"), L"");
    CHECK_EQ(childToward(L"C:\\Use", L"C:\\Users\\me"), L"");
    CHECK_EQ(childToward(L"C:\\a", L"D:\\a\\b"), L"");
    CHECK_EQ(childToward(L"\\\\server\\share", L"\\\\server\\share\\x\\y"), L"x");

    // isAbsolutePath: paths go to the folder loader, everything else is run as a command
    CHECK(isAbsolutePath(L"C:\\Windows"));
    CHECK(isAbsolutePath(L"D:\\"));
    CHECK(isAbsolutePath(L"\\\\server\\share"));
    CHECK(!isAbsolutePath(L"cmd"));
    CHECK(!isAbsolutePath(L"https://example.com"));
    CHECK(!isAbsolutePath(L"C:"));

    // isRelativePath: typed text that means a place in the current folder (after normalizePath), not a command
    CHECK(isRelativePath(L"."));
    CHECK(isRelativePath(L".\\sub"));
    CHECK(isRelativePath(L"..\\"));
    CHECK(isRelativePath(L"..\\other"));
    CHECK(isRelativePath(L"sub\\deeper"));
    CHECK(isRelativePath(L"a\\b.txt"));                    // a relative file: the loader opens it with Windows
    CHECK(isRelativePath(normalizePath(L"./sub")));
    CHECK(!isRelativePath(L"sub"));                      // a bare name: the list decides (hasFolder)
    CHECK(!isRelativePath(L"cmd"));
    CHECK(!isRelativePath(normalizePath(L"cmd /c dir")));
    CHECK(!isRelativePath(L"notepad a\\b.txt"));
    CHECK(!isRelativePath(normalizePath(L"https://example.com")));
    CHECK(!isRelativePath(L"shell:Downloads"));
    CHECK(!isRelativePath(L"C:\\Windows"));
    CHECK(!isRelativePath(L"\\\\server\\share"));
    CHECK(!isRelativePath(L""));

    // parseCommandLine: 7z-style switches, plus plain folders
    {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        CommandLine c = parseCommandLine({L".", L"-tC:\\A", L"-tc:\\a", L"-tC:\\My Files\\"});
        CHECK(c.error.empty() && !c.help && !c.selfTest);   // c:\a is C:\A again: kept once
        CHECK(c.folders.size() == 3);
        CHECK_EQ(c.folders.size() == 3 ? c.folders[0] : L"", normalizePath(cwd));
        CHECK_EQ(c.folders.size() == 3 ? c.folders[1] : L"", L"C:\\A");
        CHECK_EQ(c.folders.size() == 3 ? c.folders[2] : L"", L"C:\\My Files");
        CHECK(parseCommandLine({}).folders.empty());
        CHECK(parseCommandLine({L"-h"}).help && parseCommandLine({L"-?"}).help);
        CHECK(parseCommandLine({L"--selftest"}).selfTest);
        CHECK_EQ(parseCommandLine({L"-x"}).error, L"Unknown switch: -x");
        CHECK(!parseCommandLine({L"-t"}).error.empty());               // -t without a folder
        CHECK(!parseCommandLine({L"-tC:\\A", L"-q"}).error.empty());
        CHECK(!parseCommandLine({L"-n"}).error.empty());               // -n no longer exists
    }

    // samePath / extensionOf
    CHECK(samePath(L"C:\\Users", L"c:\\USERS"));
    CHECK(!samePath(L"C:\\Users", L"C:\\User"));
    CHECK_EQ(extensionOf(L"Photo.JPG"), L".jpg");
    CHECK_EQ(extensionOf(L"README"), L"");
    CHECK(isShortcut(L"C:\\Bookmarks\\Work.LNK") && isShortcut(L"site.url") && !isShortcut(L"notes.txt"));

    // sortItems: folders first, natural number order, ties by name
    {
        std::vector<Item> v = {file(L"file10.txt", 5), file(L"b", 0, true), file(L"file2.txt", 50),
                               file(L"a", 0, true), file(L"notes.md", 1)};
        sortItems(v, SortKey::Name, true);
        CHECK_EQ(names(v), L"a,b,file2.txt,file10.txt,notes.md");
        sortItems(v, SortKey::Name, false);
        CHECK_EQ(names(v), L"b,a,notes.md,file10.txt,file2.txt");
        sortItems(v, SortKey::Size, false);
        CHECK_EQ(names(v), L"b,a,file2.txt,file10.txt,notes.md");
        sortItems(v, SortKey::Size, true);   // 7.1: each column in both directions, folders first either way
        CHECK_EQ(names(v), L"a,b,notes.md,file10.txt,file2.txt");
        sortItems(v, SortKey::Type, true);
        CHECK_EQ(names(v), L"a,b,notes.md,file2.txt,file10.txt");
        sortItems(v, SortKey::Type, false);
        CHECK_EQ(names(v), L"b,a,file10.txt,file2.txt,notes.md");
    }

    // sortItems by date, both directions; equal dates fall back to the name
    {
        std::vector<Item> v = {dated(L"x.txt", 3), dated(L"y.txt", 1), dated(L"p.txt", 2), dated(L"q.txt", 2),
                               file(L"a", 0, true)};
        sortItems(v, SortKey::Modified, true);
        CHECK_EQ(names(v), L"a,y.txt,p.txt,q.txt,x.txt");
        sortItems(v, SortKey::Modified, false);
        CHECK_EQ(names(v), L"a,x.txt,q.txt,p.txt,y.txt");
    }

    // Names outside ASCII (Chinese, an emoji as a surrogate pair) and a path far beyond MAX_PATH
    CHECK_EQ(leafName(L"C:\\\u5831\u544A\\\U0001F600.JPG"), L"\U0001F600.JPG");
    CHECK_EQ(parentPath(L"C:\\\u5831\u544A\\\U0001F600.JPG"), L"C:\\\u5831\u544A");
    CHECK_EQ(joinPath(L"C:\\\u5831\u544A", L"\U0001F600.JPG"), L"C:\\\u5831\u544A\\\U0001F600.JPG");
    CHECK_EQ(extensionOf(L"\U0001F600.JPG"), L".jpg");
    CHECK_EQ(normalizePath(L"c:/\u5831\u544A/"), L"C:\\\u5831\u544A");
    {
        std::wstring deep = L"C:\\" + std::wstring(300, L'a') + L"\\b";   // 305 characters
        CHECK_EQ(normalizePath(deep + L"\\"), deep);
        CHECK_EQ(parentPath(deep), deep.substr(0, 303));
        CHECK_EQ(leafName(deep), L"b");
    }

    // sortItems by Type uses the type name shown in the list (when known), like Explorer
    {
        std::vector<Item> v = {file(L"b.txt"), file(L"a.zip"), file(L"c.docx")};
        v[0].type = L"Text Document";
        v[1].type = L"Compressed (zipped) Folder";
        v[2].type = L"Microsoft Word Document";
        sortItems(v, SortKey::Type, true);
        CHECK_EQ(names(v), L"a.zip,c.docx,b.txt");
    }

    // rowAfterDeleting: the item that takes the first deleted one's place, else the one before it
    CHECK(rowAfterDeleting({3, 4, 5}, 10) == 6);
    CHECK(rowAfterDeleting({9}, 10) == 8);
    CHECK(rowAfterDeleting({0, 9}, 10) == 1);   // not contiguous: row 1 moves into row 0's place
    CHECK(rowAfterDeleting({2, 7}, 10) == 3);
    CHECK(rowAfterDeleting({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 10) == -1);
    CHECK(rowAfterDeleting({}, 5) == -1);

    // matchesFilter
    CHECK(matchesFilter(file(L"Holiday Photo.jpg"), L"photo"));
    CHECK(matchesFilter(file(L"Holiday Photo.jpg"), L""));
    CHECK(!matchesFilter(file(L"Holiday Photo.jpg"), L"video"));
    CHECK(matchesFilter(file(L"report.PDF"), L"*.pdf"));
    CHECK(!matchesFilter(file(L"report.pdf.txt"), L"*.pdf"));

    // listFolder
    {
        Listing l = listFolder(L"C:\\Windows", false);
        CHECK(l.error == 0);
        CHECK(!l.items.empty());
        bool dots = false;
        for (const auto& it : l.items) dots |= it.name == L"." || it.name == L"..";
        CHECK(!dots);
        CHECK(listFolder(L"C:\\definitely\\not\\here", false).error != 0);
        CHECK(!listFolder(L"", false).items.empty());   // at least one drive
        // A path that is a file is reported as such (the app then opens the file instead).
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        Listing asFile = listFolder(exe, false);
        CHECK(asFile.error != 0 && asFile.isFile);
        CHECK(!listFolder(L"C:\\definitely\\not\\here", false).isFile);
        // A read that a newer load replaced stops early, keeping nothing (System32 has hundreds of entries).
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        Listing stopped = listFolder(sys, false, [] { return true; });
        CHECK(stopped.error == ERROR_OPERATION_ABORTED && stopped.items.empty());
        CHECK(listFolder(sys, false, [] { return false; }).items.size() > 256);
    }

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
