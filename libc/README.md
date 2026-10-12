# Bundled C libraries

This directory holds what the compiler needs to link a program for a platform
other than the one it runs on, without any toolchain, SDK or sysroot of that
platform being installed:

```sh
odin build . -target:linux_amd64                    # glibc, dynamically linked
odin build . -target:linux_arm64 -libc:musl         # musl, fully static
odin build . -target:x86_64-linux-gnu.2.28          # runs on glibc 2.28 and newer
odin build . -target:windows_amd64                  # mingw-w64 + UCRT
odin build . -target:darwin_arm64                   # libSystem
odin build . -target:freebsd_amd64
```

The pieces of a C library which a program needs are built on demand for the
exact target, with `clang`, the first time they are needed, and are kept in a
global cache. The program is then linked by calling LLD directly. The compiler
side of this lives in `src/cross_libc.cpp`.

The approach, the build recipes and almost all of the files in here are those
of the [Zig](https://ziglang.org) compiler, which has been doing this for years.

## What gets used when

A bundled libc is the default when cross compiling to a target which has one.
`-libc:<name>` chooses explicitly, also for a native build; `-libc:system`
gives the linking back to the toolchain of the system.

| Target                                | libc                    | Linked     |
|---------------------------------------|-------------------------|------------|
| `linux_{amd64,i386,arm64,arm32,riscv64}` | `glibc` (default), any version up to 2.42; the oldest is 2.2.5 for `amd64` and `i386`, 2.4 for `arm32`, 2.17 for `arm64` and 2.27 for `riscv64` | dynamically |
| `linux_{amd64,i386,arm64,arm32,riscv64}` | `musl` 1.2.5         | statically (default) or dynamically with `-link-mode:dynamic` |
| `windows_amd64`                       | `mingw` (mingw-w64, UCRT) | dynamically against the DLLs of Windows |
| `darwin_{amd64,arm64}`                | `libsystem`             | dynamically |
| `freebsd_{amd64,arm64}`               | `freebsd` 14.0 and newer | dynamically |
| `netbsd_{amd64,arm64}`                | `netbsd` 10.1           | dynamically |

The version of glibc defaults to 2.31 (2.34 for `i386` and `arm32`) and is given with the libc (`-libc:glibc.2.28`) or with the
target (`-target:x86_64-linux-gnu.2.28`). The version of FreeBSD, NetBSD and
macOS is the one of `-minimum-os-version`.

Things to know:

* `clang` and `lld` have to be installed. They are looked for in `PATH`, or
  taken from the `ODIN_CLANG_PATH` and `ODIN_LLD_PATH` environment variables.
  On Windows the `lld-link.exe` which ships with the compiler is used.
* The cache is in `~/.cache/odin` (`~/Library/Caches/odin` on macOS,
  `%LOCALAPPDATA%\odin\cache` on Windows), or in the directory of the
  `ODIN_CACHE_DIR` environment variable. It is safe to delete at any time.
* Only the C library is bundled. A `foreign import` of any other system library
  needs that library for the target, pass its directory with
  `-extra-linker-flags:"-L<dir>"`.
* On macOS only `libSystem` is available. Frameworks are a part of the macOS
  SDK, which cannot be redistributed; set the `SDKROOT` environment variable to
  an SDK to link against them.
* On Windows the import library of a DLL is generated from the `.def` files of
  mingw-w64, which cover the DLLs that come with Windows. `.rc` resources cannot
  be compiled without the Windows SDK, use a precompiled `.res`.
* The sanitizers (`-sanitize`) are not available, their runtimes are not bundled.

## How it works

* **glibc, FreeBSD, NetBSD** are only ever linked dynamically, so all the
  linker needs of them is the list of the symbols they export. `abilists` is a
  compact encoding of the symbols of every version and every target of the
  libc. Out of it an assembly file which defines the symbols of the wanted
  version is generated for each of the libraries (`libc.so.6`, `libm.so.6`, ...)
  and linked into a shared library that stands in for the real one. The start
  files (`crt1.o`, ...) are built from source.
* **musl** is built from source in full, into a static library, or stands in
  as a stub (`musl/libc.S`) when it is linked dynamically.
* **mingw-w64**: the runtime (`crt2.o`, `libmingw32`) is built from source, the
  import libraries of the DLLs out of their `.def` files.
* **compiler-rt**: the "builtins" which LLVM calls for the operations a CPU has
  no instructions for (64-bit division on a 32-bit CPU, 128-bit floats, ...),
  built from source. A native build gets them out of the `libgcc` of the system.
* **Darwin**: `libSystem.tbd` is the list of the symbols of libSystem, which the
  linker understands as it is.

## Layout and origin

| Directory     | Origin |
|---------------|--------|
| `glibc/`, `musl/`, `mingw/`, `freebsd/`, `netbsd/`, `darwin/` | `lib/libc` of Zig, unmodified, see `LICENSE-zig` and the licenses inside each of them |
| `include/`    | `lib/libc/include` of Zig: the headers needed to build the above, for the architectures Odin supports |
| `musl/src/{math,stdlib,string}` | a few files which Zig replaced with Zig code are restored from musl 1.2.5, they are listed under `[restored]` in `musl/sources.txt` |
| `compiler_rt/builtins/` | `compiler-rt/lib/builtins` of LLVM 21.1.0, see `compiler_rt/LICENSE.TXT` |
| `compiler_rt/odin/` | written for Odin |
| `*/sources.txt` | generated, the lists of the files which get built |

## Updating

```sh
# The libcs, from a checkout of Zig. The second argument is the source of the
# release of musl that Zig is at, and is only needed when musl got updated.
cp -r <zig>/lib/libc/{glibc,musl,mingw,freebsd,netbsd,darwin} libc/
python3 libc/tools/update_from_zig.py <zig> [<musl>]

# compiler-rt, from the source of a release of LLVM
python3 libc/tools/update_compiler_rt.py <compiler-rt>
```

The directories of `include/` are copied from `<zig>/lib/libc/include` as they
are. `CROSS_LIBC_CACHE_REVISION` in `src/cross_libc.cpp` has to be bumped when
the way something is built changes, so that what is in the caches gets rebuilt.

`tests/cross/run.sh` builds a test program for every one of the targets and
runs the ones it is able to.
