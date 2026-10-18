# Building Qoqdir

Qoqdir is one program in two versions: C++ in `src/`, the one that ships, and
Rust in `rust/`. Both build on Windows 10 or 11 (x64) with tools that live in
their own folders: nothing is installed system-wide and no Visual Studio is
needed.

In short, once the tools are set up (section 1):

```powershell
powershell -File scripts\build.ps1          # build both versions
powershell -File scripts\build.ps1 -Test    # and run every check
```

| File | What it is |
|---|---|
| `build\qoqdir.exe` | the program, what users get |
| `build\qoqdir-test.exe` | the same compiled program plus the self-test (`--selftest`) |
| `build\unit_tests.exe` | the C++ unit tests |
| `rust\target\release\qoqdir.exe` | the Rust version |
| `rust\target\selftest\release\qoqdir.exe` | the Rust version with the self-test |

## 1. Tools

Set these up once.

### MinGW-w64, for the C++ version

GCC 13 with CMake and Ninja, in one archive from winlibs. The releases are
built with it.

1. Download `winlibs-x86_64-posix-seh-gcc-13.2.0-mingw-w64ucrt-11.0.1-r8.zip`
   (216 MB) or the `.7z` (94 MB) from
   https://github.com/brechtsanders/winlibs_mingw/releases/tag/13.2.0posix-18.1.5-11.0.1-ucrt-r8.
   A `.sha256` file is beside it. On winlibs.com it is the Win64 link "without
   LLVM/Clang/LLD/LLDB" under GCC 13.2.0, UCRT, release 8: Win64, UCRT runtime,
   POSIX threads.
2. Unpack it. The archive holds one folder, `mingw64`. Put it in
   `%LOCALAPPDATA%\Programs`, so that
   `%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe` exists. No admin rights are
   needed.
3. In each new terminal:

```powershell
$env:PATH = "$env:LOCALAPPDATA\Programs\mingw64\bin;" + $env:PATH
g++ --version
```

`scripts\build.ps1` and `scripts\release.ps1` do step 3 themselves: when g++
isn't on the PATH they look in `%LOCALAPPDATA%\Programs\mingw64`, then in
`C:\mingw64`.

Any other folder works if its path has no spaces, which MinGW's tools dislike.
With a space in your user name, use `C:\mingw64`: any user may create a folder
at the root of `C:`. The compiler's `windres` also puts the icon into the Rust
exe. MSYS2's UCRT64 GCC works too (the build on GitHub uses it).

### Rust, for the Rust version

A portable install for the GNU target, in `%LOCALAPPDATA%\Programs\rust`. No
admin rights, and nothing else on the system changes.

1. Install, in PowerShell. It downloads about 200 MB and takes a few minutes:

   ```powershell
   $R = "$env:LOCALAPPDATA\Programs\rust"; New-Item -ItemType Directory -Force $R
   Invoke-WebRequest https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-gnu/rustup-init.exe -OutFile $R\rustup-init.exe
   $env:RUSTUP_HOME = "$R\rustup"; $env:CARGO_HOME = "$R\cargo"
   & $R\rustup-init.exe -y --no-modify-path --profile minimal -c clippy --default-host x86_64-pc-windows-gnu --default-toolchain stable
   ```

2. Save these four lines as `rust-env.ps1` in that folder. The last one keeps
   the folder's path out of the exe (section 2), and the same flag for every
   build, so that nothing is built twice:

   ```powershell
   $env:RUSTUP_HOME = Join-Path $PSScriptRoot 'rustup'
   $env:CARGO_HOME  = Join-Path $PSScriptRoot 'cargo'
   $env:PATH = (Join-Path $PSScriptRoot 'cargo\bin') + ';' + $env:PATH
   $env:RUSTFLAGS = "--remap-path-prefix=$env:CARGO_HOME=cargo"
   ```

3. In each new terminal: `. $env:LOCALAPPDATA\Programs\rust\rust-env.ps1`.
   `scripts\build.ps1` does this itself: when cargo isn't on the PATH it looks
   there, then in `C:\rust`.

The code needs Rust 1.91 or newer (`rust-version` in `Cargo.toml`, which clippy
checks the code against). `rustup update` updates Rust; deleting the folder
removes it.

### PowerShell scripts

The scripts in `scripts\` and `tests\` run in Windows' own PowerShell. Windows
runs no `.ps1` file until you allow it, once for your account:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

Or start each one with `powershell -ExecutionPolicy Bypass -File ...` instead.

## 2. Build

### Both versions at once

```powershell
powershell -File scripts\build.ps1
```

It takes the tools from the PATH, or else from `%LOCALAPPDATA%\Programs\mingw64`
or `C:\mingw64` and from `%LOCALAPPDATA%\Programs\rust` or `C:\rust`, and
builds the C++ version (a compiler warning stops it) and both Rust builds.

### The C++ version by hand

```powershell
cmake -S . -B build -G Ninja      # once; Release unless told otherwise
cmake --build build
```

`qoqdir.exe` is statically linked: one file of about 570 KB, with no DLLs beside
it. GCC's runtime checks are on, so a bad index or a signed integer overflow
stops the program and writes the reason to `%TEMP%\qoqdir-crash.log` instead of
running on with wrong data. A build to step through with `gdb` (it comes with
winlibs) goes in a folder of its own:
`cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug`.

### The C++ version with GCC alone

On a PC with just the compiler, no CMake or Ninja: `windres` and `g++` come with
any MinGW-w64 GCC. It needs only the files in `src\` (not `selftest.cpp`, which
goes into the test version only) and `res\`. In `src\`, with the compiler's
`bin` on the PATH (section 1):

```powershell
windres ..\res\qoqdir.rc -O coff -o app_res.o
g++ -std=c++20 -Werror -O2 -DNDEBUG -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -D_GLIBCXX_ASSERTIONS -fsanitize=signed-integer-overflow -fsanitize-undefined-trap-on-error -ffunction-sections -fdata-sections -mwindows -municode -static '-Wl,--gc-sections' -s main.cpp app.cpp filelist.cpp tree.cpp shell.cpp folder.cpp crashlog.cpp app_res.o -o qoqdir.exe -lole32 -loleaut32 -lshell32 -lshlwapi -lcomctl32 -luuid
```

That makes the same `qoqdir.exe` as the CMake build, in about two minutes. The
flags are the ones in `CMakeLists.txt`: keep the two in step. CMake adds one
more, `-ffile-prefix-map=<your mingw64 folder>=mingw64` with the folder's full
path in forward slashes, so that the runtime checks' messages in the exe name no
local folder (and no user name, when the compiler is under `C:\Users`); add it
to publish an exe built this way. PowerShell needs
the quotes around `-Wl,--gc-sections`, as it reads a comma as a list; cmd.exe
doesn't.

### The Rust version by hand

From `rust\`, with the compiler's `bin` on the PATH for the icon (section 1):

```powershell
cargo build --release                                                    # target\release\qoqdir.exe
cargo build --release --features selftest --target-dir target\selftest   # the same with the self-test
```

The self-test build gets its own folder, so it never replaces your exe.

The crates' panic messages name their source files, which live under the
Rust folder's `cargo\registry`: on a per-user install that path holds your
user name. The `RUSTFLAGS` line in `rust-env.ps1` (section 1) maps that folder
to `cargo`, so the exe holds only `cargo\...`; `scripts\build.ps1` sets the
same flag when it isn't set. Cargo rebuilds every crate when the flags change,
so keep it set for builds by hand too.

## 3. Test

Run every check before calling a change done:

```powershell
powershell -File scripts\build.ps1 -Test
```

After building, it runs these in order and stops at the first failure:

1. the unit tests of both versions;
2. clippy on both Rust builds, which must report nothing;
3. the command-line checks on both exes;
4. the self-test of each version, one after the other, with the popup watch;
5. a comparison of the two self-test logs: both versions must have taken the
   same steps with the same results.

It takes about ten minutes, most of it the two self-tests.

What each one is:

- Unit tests: paths, the command line, sorting, filtering and reading folders.
  The C++ ones (`tests\unit-tests.cpp`) are one `main()` of `CHECK` macros that prints
  `FAIL line N` and keeps going; the Rust ones are at the end of
  `rust\src\folder.rs`.
- The self-test drives the real program through every scenario in section 7.2 of
  the spec, from inside its own process: its window doesn't take the focus, and
  it never sends keys to other windows. It works in its own folder in `%TEMP%`,
  never touches your `qoqdir.ini` or Bookmarks, replaces the clipboard and leaves
  one small file in the Recycle Bin. Each step has a timeout, and a failure
  doesn't stop the run: the log (`%TEMP%\qoqdir-selftest.log`, for Rust
  `%TEMP%\qoqdir-rs-selftest.log`) gets an `ok` or `FAIL` line as each step ends.
  There is no way to run one step alone.
- `tests\exe-checks.ps1` checks the exe that ships: one file under 1 MB that
  imports only DLLs Windows protects (spec 3). It needs MinGW's `objdump` on
  the PATH.
- `tests\cli-checks.ps1` starts a copy of an exe in a temp folder (so your
  settings stay untouched) with several command lines, a damaged `qoqdir.ini` and
  a folder it can't write to.
- `tests\watch-windows.ps1` runs a self-test and lists every window that pops up
  meanwhile, from any program. It only reports: it never closes a window.

The same checks by hand, from the repository folder:

```powershell
build\unit_tests.exe
powershell -File tests\exe-checks.ps1 -Exe build\qoqdir.exe
powershell -File tests\cli-checks.ps1 -Exe build\qoqdir.exe
powershell -File tests\watch-windows.ps1 -Exe build\qoqdir-test.exe
cd rust
cargo test
cargo clippy --all-targets -- -D warnings
cargo clippy --all-targets --features selftest --target-dir target\selftest -- -D warnings
cd ..
powershell -File tests\cli-checks.ps1 -Exe rust\target\release\qoqdir.exe
powershell -File tests\watch-windows.ps1 -Exe rust\target\selftest\release\qoqdir.exe -Log $env:TEMP\qoqdir-rs-selftest.log
```

(`ctest --test-dir build --output-on-failure` runs the C++ unit tests and
self-test, without the popup watch.)

Run the self-tests on a quiet PC, never two at once. A step that fails while you
were copying files or switching windows should pass when run again; rerun it
before blaming the code.

### Deeper checks

Two builds with extra runtime checks catch mistakes the normal build lets pass.
Each takes a few minutes; then run its self-test with `tests\watch-windows.ps1`.

- C++ with the undefined-behaviour checker, a stack protector and libstdc++'s
  debug mode. Debug mode brings its own `__glibcxx_assert_fail`, which
  `crashlog.cpp` replaces, hence the linker flag:

  ```powershell
  cmake -S . -B build-checked -G Ninja "-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize=vptr -fsanitize-undefined-trap-on-error -fstack-protector-strong -D_GLIBCXX_DEBUG" "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--allow-multiple-definition"
  cmake --build build-checked
  powershell -File tests\watch-windows.ps1 -Exe build-checked\qoqdir-test.exe
  ```

- Rust with its debug assertions in a release build, from `rust\`:

  ```powershell
  $env:CARGO_PROFILE_RELEASE_DEBUG_ASSERTIONS = 'true'
  cargo build --release --features selftest --target-dir target\checked
  Remove-Item Env:CARGO_PROFILE_RELEASE_DEBUG_ASSERTIONS
  powershell -File ..\tests\watch-windows.ps1 -Exe target\checked\release\qoqdir.exe -Log $env:TEMP\qoqdir-rs-selftest.log
  ```

The self-test must also pass at 100% scaling. On a screen scaled higher, run it
on a copy of `qoqdir-test.exe` whose Properties, Compatibility, "Change high DPI
settings" has "Override high DPI scaling behavior" ticked, scaled by System.

## 4. The icon

`res\qoqdir.ico` is drawn by `scripts\make-icon.ps1`: a yellow folder with a red
bookmark ribbon over its front, and Qoqdir's mark, a white Q, on the ribbon.
Every shape is drawn in the script from numbers, nothing is taken from another
icon, and there is a picture for each size Windows asks for at 100 to 200%
scaling, the small ones set pixel by pixel. After changing it, run
`powershell -File scripts\make-icon.ps1`; the next build of either version takes
the new icon in.

## 5. Release

Only the C++ version ships. Raise the version in `src\version.h` (both lines)
and in `rust\Cargo.toml`, commit, then:

```powershell
powershell -File scripts\release.ps1
```

It stops if the three versions disagree (both lines of `version.h`, and
`Cargo.toml`), builds that commit in a clean copy, runs the unit tests, the exe
checks and the command-line checks on the exe that ships and the self-test on
`qoqdir-test.exe`, and leaves in `release\`:

- `qoqdir-<version>.zip` to publish (the exe, README, LICENSE and the
  third-party notices); it prints the zip's SHA-256 for the release notes.
- `qoqdir-<version>-debug.exe` to keep: the same exe with its symbols, which turns
  the offsets in a crash log from that version into source lines
  (`addr2line -e qoqdir-<version>-debug.exe 0x140000000+offset`).

## 6. The build on GitHub

`.github\workflows\build.yml` runs on every push and pull request. It builds the
C++ version with MSYS2's GCC on a Windows runner (a warning fails it there too,
`-Werror` being in `CMakeLists.txt`), then runs the same checks as
`scripts\build.ps1 -Test` for that version: the unit tests, `tests\exe-checks.ps1`,
the self-test with the popup watch and the command-line checks. The exe and the
logs are kept with the run. The Rust version isn't built there.

## 7. When something goes wrong

- "running scripts is disabled on this system": see PowerShell scripts in
  section 1.
- Cargo says `windres not found: the exe gets no icon`: the compiler's `bin` is
  not on the PATH (`scripts\build.ps1` adds it, section 1).
- The Rust build fails in `dlltool` (it did on the first build machine): point
  cargo at MinGW's, with the full path of `dlltool.exe` in your `mingw64\bin`,
  in `cargo\config.toml` inside the Rust folder:

  ```toml
  [target.x86_64-pc-windows-gnu]
  rustflags = ["-C", "dlltool=C:\\Users\\<you>\\AppData\\Local\\Programs\\mingw64\\bin\\dlltool.exe"]
  ```

- Explorer still shows an exe's old icon: Windows keeps icons in a cache, which
  catches up after a while.
- A program that stops on a bug, or a failed runtime check, adds a line to
  `%TEMP%\qoqdir-crash.log` saying why.

The sources also cross-build on Linux with MinGW-w64, and Wine can run the
tests, but only as a quick check.
