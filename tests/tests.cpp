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
        sortItems(v, SortKey::Type, true);
        CHECK_EQ(names(v), L"a,b,notes.md,file2.txt,file10.txt");
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
    }

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
