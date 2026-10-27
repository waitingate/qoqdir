// Tests for the path and folder rules. Run: ctest --test-dir build  (or build\unit_tests.exe)
#include "crashlog.h"
#include "folder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

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

// Runs this program again with one argument and TEMP moved to `temp` (the child inherits it); its exit code, or -1.
static int runSelf(const wchar_t* arg, const std::wstring& temp) {
    std::wstring cmd = L"\"" + exePath() + L"\" " + arg;
    SetEnvironmentVariableW(L"TEMP", temp.c_str());
    SetEnvironmentVariableW(L"TMP", temp.c_str());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

static std::string readFile(const std::wstring& path) {
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        fclose(f);
    }
    return text;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--throw") == 0) {   // the crash-log test below: an exception nothing catches
        installCrashLog();
        throw std::out_of_range("map::at");
    }
    if (argc > 1 && strcmp(argv[1], "--throw-in-thread") == 0) {   // the same on a thread of its own
        installCrashLog();
        HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD { throw std::out_of_range("map::at"); }, nullptr, 0, nullptr);
        WaitForSingleObject(t, INFINITE);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--parent") == 0) {   // the parent-process test below: started by this program
        return parentProcessName() == toLower(leafName(exePath())) ? 0 : 5;
    }
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
    // A name from the root, as typed ("\Windows"): on the folder's own drive or share, as Explorer takes it (5.3).
    CHECK_EQ(joinPath(L"C:\\x\\y", L"\\Windows"), L"C:\\Windows");
    CHECK_EQ(joinPath(L"C:\\", L"\\Windows"), L"C:\\Windows");
    CHECK_EQ(joinPath(L"C:\\x", L"\\"), L"C:\\");
    CHECK_EQ(joinPath(L"\\\\server\\share\\dir", L"\\x"), L"\\\\server\\share\\x");
    CHECK_EQ(joinPath(L"\\\\server\\share", L"\\x"), L"\\\\server\\share\\x");
    CHECK_EQ(leafName(L"C:\\a\\b"), L"b");
    CHECK_EQ(leafName(L"C:\\"), L"C:\\");
    CHECK_EQ(leafName(L"\\\\server\\share"), L"share");
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
        CHECK(samePath(normalizePath(expandVars(L"%SystemRoot%")), windir));
    }
    // A folder may really be named "%TEMP%": only typed text has its variables expanded (P14).
    CHECK_EQ(normalizePath(L"C:\\a\\%TEMP%\\.\\sub"), L"C:\\a\\%TEMP%\\sub");
    CHECK_EQ(resolveArgument(L"C:\\a\\%TEMP%"), L"C:\\a\\%TEMP%");   // "qoqdir ." in such a folder

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
    // A server without a share is a (bad) network path, not a place here: the same rule in both versions.
    CHECK(isAbsolutePath(L"\\\\server") && !isRelativePath(L"\\\\server") && isAbsolutePath(L"C:\\"));
    CHECK(!isAbsolutePath(L"C:a") && !isAbsolutePath(L"\\a") && !isAbsolutePath(L"cmd") && !isAbsolutePath(L""));

    // parseCommandLine: switches, plus plain folders
    {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        CommandLine c = parseCommandLine({L".", L"--tab=C:\\A", L"--tab=c:\\a", L"-t=C:\\My Files\\"});
        CHECK(c.error.empty() && !c.help && !c.selfTest);   // c:\a is C:\A again: kept once
        CHECK(c.folders.size() == 3);
        CHECK_EQ(c.folders.size() == 3 ? c.folders[0] : L"", normalizePath(cwd));
        CHECK_EQ(c.folders.size() == 3 ? c.folders[1] : L"", L"C:\\A");
        CHECK_EQ(c.folders.size() == 3 ? c.folders[2] : L"", L"C:\\My Files");
        CHECK(parseCommandLine({}).folders.empty());
        CHECK(parseCommandLine({L"-h"}).help && parseCommandLine({L"-?"}).help);
        CHECK(parseCommandLine({L"--selftest"}).selfTest);
        CHECK_EQ(parseCommandLine({L"-x"}).error, L"Unknown option: -x");
        // A network folder comes out as itself, and a name that starts with "-" is a folder too.
        CHECK_EQ(parseCommandLine({L"--tab=\\\\nas\\share\\"}).folders.at(0), L"\\\\nas\\share");
        // cmd turns `qoqdir "C:\"` into the argument C:" (trailing quote): cleaned to the drive root, not C:'s cwd.
        CHECK_EQ(parseCommandLine({L"C:\""}).folders.at(0), L"C:\\");
        CommandLine dash = parseCommandLine({L"--tab=-h"});
        CHECK(dash.error.empty() && !dash.help && dash.folders.size() == 1 && dash.folders[0].ends_with(L"\\-h"));
        // A trailing backslash is the same folder (one tab), and -t= takes a name starting with "-" as a folder too.
        CHECK(parseCommandLine({L"C:\\A", L"-t=C:\\A\\"}).folders.size() == 1);
        CommandLine td = parseCommandLine({L"-t=-h"});
        CHECK(td.error.empty() && td.folders.size() == 1 && td.folders[0].ends_with(L"\\-h"));
        CHECK(!parseCommandLine({L"--tab=C:\\A", L"-q"}).error.empty());
        CHECK(!parseCommandLine({L"-n"}).error.empty());               // -n no longer exists
        CHECK(!parseCommandLine({L"-tC:\\A"}).error.empty());          // nor -t without "="
    }

    // tabArgument: the folder a new window gets (5.8) comes back whole through Windows' command-line rules, a root's
    // closing backslash and a name that looks like an option or a variable included.
    for (const wchar_t* folder : {L"C:\\My Files", L"D:\\", L"\\\\nas\\share", L"C:\\-h", L"C:\\x\\%TEMP%",
                                  L"C:\\\u5831\u544A\\new folder"}) {
        std::vector<std::wstring> words = commandLineWords(L"qoqdir.exe " + tabArgument(folder));
        CHECK(words.size() == 2);
        CommandLine c = parseCommandLine(std::vector<std::wstring>(words.begin() + 1, words.end()));
        CHECK(c.error.empty() && c.folders.size() == 1);
        CHECK_EQ(c.folders.empty() ? L"" : c.folders[0], folder);
    }
    // The parser would mend "D:\" (a stray closing quote goes, and D: means D:\), but the quoting is right by itself.
    CHECK_EQ(tabArgument(L"D:\\"), L"--tab=\"D:\\\\\"");

    // cascade: a title bar down and right, the same size; back at the work area's top left where it would leave it.
    {
        auto same = [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != 0; };
        RECT work{0, 0, 1920, 1040};
        CHECK(same(cascade({100, 100, 900, 700}, work, 30), {130, 130, 930, 730}));
        CHECK(same(cascade({100, 400, 900, 1010}, work, 30), {130, 430, 930, 1040}));   // just fits
        CHECK(same(cascade({100, 420, 900, 1020}, work, 30), {0, 0, 800, 600}));        // past the bottom
        CHECK(same(cascade({1500, 100, 1900, 700}, work, 30), {0, 0, 400, 600}));       // past the right edge
        CHECK(same(cascade({0, 0, 1920, 1040}, work, 30), {0, 0, 1920, 1040}));         // as big as the work area
        // A monitor left of the main one, with its taskbar at the top: its work area doesn't start at 0.
        RECT left{-1920, 40, 0, 1080};
        CHECK(same(cascade({-1800, 100, -1000, 700}, left, 30), {-1770, 130, -970, 730}));
        CHECK(same(cascade({-1800, 20, -1000, 620}, left, 10), {-1920, 40, -1120, 640}));   // above the work area
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
        std::vector<Item> v = {file(L"b.txt"), file(L"a.zip"), file(L"c.docx"), file(L"a.txt"), file(L"c.txt")};
        v[0].type = L"Text Document";
        v[1].type = L"Compressed (zipped) Folder";
        v[2].type = L"Microsoft Word Document";
        v[3].type = L"Text Document";   // the same type: by name
        v[4].type = L"text document";   // the same but for letter case: by name too
        sortItems(v, SortKey::Type, true);
        CHECK_EQ(names(v), L"a.zip,c.docx,a.txt,b.txt,c.txt");
        sortItems(v, SortKey::Type, false);
        CHECK_EQ(names(v), L"c.txt,b.txt,a.txt,c.docx,a.zip");
    }

    // sortItems orders names exactly as Windows' StrCmpLogicalW (Explorer) does, tricky names included
    {
        std::vector<std::wstring> want = {L"file10", L"file2", L"File1", L"file01", L"file 1", L"file_1", L"file-1",
            L"file1a", L"a", L"_a", L"(a)", L"!a", L"a b", L"New folder (10)", L"New folder", L"New folder (2)", L"v1.10",
            L"v1.2.9", L"報告10.pdf", L"報告2.pdf", L"１２", L"äbc", L"abc", L"straße",
            L"strasse", L"\U0001F600.jpg", L"12345678901234567891", L"12345678901234567890", L"IMG_0100.JPG", L"img_0099.jpg"};
        std::vector<Item> v;
        for (const std::wstring& n : want) v.push_back(file(n.c_str()));
        sortItems(v, SortKey::Name, true);
        std::stable_sort(want.begin(), want.end(),
                         [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
        std::wstring expected;
        for (const std::wstring& n : want) expected += (expected.empty() ? L"" : L",") + n;
        CHECK_EQ(names(v), expected);
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
    // Letters beyond English, and their case: Windows' own matcher missed these under a Chinese code page.
    CHECK(matchesFilter(file(L"資料.txt"), L"資*"));
    CHECK(matchesFilter(file(L"ÄBC.txt"), L"äbc*"));
    CHECK(matchesFilter(file(L"a.png"), L"*.jpg; *.png") && !matchesFilter(file(L"a.gif"), L"*.jpg;*.png"));
    CHECK(matchesFilter(file(L"abc"), L"a?c") && !matchesFilter(file(L"ac"), L"a?c"));
    {
        // Windows' matcher took 8.8 s here, 4 to 9 times longer with each star; the filter runs it on every file.
        ULONGLONG start = GetTickCount64();
        CHECK(!matchesFilter(file(std::wstring(40, L'a').c_str()), L"*a*a*a*a*a*a*a*b"));
        CHECK(GetTickCount64() - start < 100);
    }

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
        Listing asFile = listFolder(exePath(), false);
        CHECK(asFile.error != 0 && asFile.isFile);
        CHECK(!listFolder(L"C:\\definitely\\not\\here", false).isFile);
        // A read that a newer load replaced stops early, keeping nothing (System32 has hundreds of entries).
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        Listing stopped = listFolder(sys, false, [] { return true; });
        CHECK(stopped.error == ERROR_OPERATION_ABORTED && stopped.items.empty());
        CHECK(listFolder(sys, false, [] { return false; }).items.size() > 256);
    }

    // dropKeys: a plain drag that could move files goes on as a right-button drag, so Windows asks (5.4); a key held, a
    // drag that can only copy, a right-drag and the drop itself (no button down) go on as they are.
    {
        DWORD any = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        CHECK(dropKeys(MK_LBUTTON, any) == MK_RBUTTON);
        CHECK(dropKeys(MK_LBUTTON, DROPEFFECT_MOVE) == MK_RBUTTON);
        CHECK(dropKeys(MK_LBUTTON | MK_CONTROL, any) == (MK_LBUTTON | MK_CONTROL));
        CHECK(dropKeys(MK_LBUTTON | MK_SHIFT, any) == (MK_LBUTTON | MK_SHIFT));
        CHECK(dropKeys(MK_LBUTTON | MK_ALT, any) == (MK_LBUTTON | MK_ALT));
        CHECK(dropKeys(MK_LBUTTON, DROPEFFECT_COPY | DROPEFFECT_LINK) == MK_LBUTTON);
        CHECK(dropKeys(MK_RBUTTON, any) == MK_RBUTTON);
        CHECK(dropKeys(0, any) == 0);
    }

    // An exception nothing catches stops the program with exit code 3, and the crash log names it (4.10).
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        std::wstring dir = joinPath(temp, L"qoqdir-unit-tests");
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring log = joinPath(dir, L"qoqdir-crash.log");
        DeleteFileW(log.c_str());
        CHECK(runSelf(L"--throw", dir) == 3);
        std::string line = readFile(log);
        CHECK(line.find("std::out_of_range") != std::string::npos && line.find("map::at") != std::string::npos);
        // The same on a thread that nothing of the startup code wraps: the exception filter sees the throw first, as
        // newer mingw-w64 has it on the main thread too.
        DeleteFileW(log.c_str());
        CHECK(runSelf(L"--throw-in-thread", dir) == 3);
        line = readFile(log);
        CHECK(line.find("std::out_of_range") != std::string::npos && line.find("map::at") != std::string::npos);
        // The program this one was started from is named (main.cpp sends PowerShell, and only PowerShell, an Enter).
        CHECK(runSelf(L"--parent", dir) == 0);
        DeleteFileW(log.c_str());
        RemoveDirectoryW(dir.c_str());
    }

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
