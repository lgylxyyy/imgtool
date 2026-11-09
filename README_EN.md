<div align="center">

# imgtool

**Read and write qcow2 images offline — partitions, filesystems, and Windows boot. No mounting, no booting.**

[**English**](README_EN.md) | [中文 🇨🇳](README.md)

[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-informational)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Android%20%7C%20arm64-informational)]()
[![Commands](https://img.shields.io/badge/commands-76-informational)]()

</div>

`imgtool` is a C11 library plus command-line tool (`imgcmd`). It treats a
`.qcow2` / `.img` / `.vhd` / `.vmdk` file as a raw block device: partition tables,
FAT32 / NTFS, WIM/ESD, ISO, archives, Windows boot chain (MBR/VBR code, BCD,
bootmgr, bootmgfw.efi, WinRE), and registry hives.

> Related repository: [Icdqcow2](https://github.com/lgylxyyy/Icdqcow2) — the Android app that embeds this library.

---

## Quick start

```bash
# Build (missing optional dependencies degrade the matching commands; the build never fails)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# Self-check
build/tools/imgcmd --list | head -c 200
# {"version":1,"features":{"wimlib":true,"ntfs3g":true,"zlib":true,"mkntfs":true},...}
```

| Section | Contents |
|---|---|
| [Building](#building) | Dependencies, host build, Android cross-compile |
| [Capabilities](#capabilities) | Supported formats and operations |
| [Command line](#command-line) | Common commands and result structure |
| [License](#license) | This project and all third-party components |

---

## Building

### Host

Requirements: CMake ≥ 3.16, a C11 compiler, `zlib`. Optional:

| Optional dependency | Commands enabled | Disable with |
|---|---|---|
| `libntfs-3g` | All NTFS read/write | `-DIMG_WITH_NTFS3G=OFF` |
| `ntfsprogs` (mkntfs) | `fs.format --fs=ntfs` | OFF by default; enable with `-DIMG_WITH_NTFS=ON` after running `tools/build-mkntfs-obj.sh` |
| `wimlib` | `wim.*` | `-DIMG_WITH_WIM=OFF` |
| `libarchive` | `archive.*` (zip/7z/tar) | `-DIMG_WITH_ARCHIVE=OFF` |
| `libhivex` | BCD / registry hives | Auto-detected |
| bundled unrar | `archive.*` reading RAR | `-DIMG_WITH_RAR=OFF` |

Artifacts: `build/tools/imgcmd`, `build/libimgtool.a`.

### Android (NDK cross-compile)

Same source tree; only the CMake toolchain file changes:

```bash
export NDK=/path/to/android-ndk-r28c

./android-deps.sh                  # one-time: cross-compile ntfs-3g/wimlib/zlib/libarchive/unrar

./android-cross.sh                 # default: arm64-v8a armeabi-v7a x86_64
./android-cross.sh arm64-v8a       # single ABI
TOOLS=1 TESTS=1 ./android-cross.sh # also build imgcmd and unit tests
NTFS=OFF ./android-cross.sh        # without GPL-licensed mkntfs
```

Artifact: `build/android/<abi>/libimgtool.so` (buildable from `API=21`; the consuming app uses `minSdk=33`).

Behavioral differences on Android (adapted in-tree via `#ifdef __ANDROID__`, 4 sites):

- Long tasks interrupt only through the **progress callback + cancel flag** (no signals, no multi-process).
- `content://` URIs are staged to cache by the app; absolute paths are passed in.
- `/sdcard` goes through FUSE and has no conventional Unix permission semantics.
- Callers must check free space (`statvfs`) before writing.

See [`ANDROID-BUILD.md`](ANDROID-BUILD.md) and [`ARMHF-BUILD.md`](ARMHF-BUILD.md).

### Branches

| Branch | Purpose |
|---|---|
| `main` | Mainline development; both host and Android targets build |
| `android` | Mobile release branch: its `libimgtool.so` is consumed by [Icdqcow2](https://github.com/lgylxyyy/Icdqcow2). Periodically merged from `main` to stay in sync |

To build the library for the app, check out `android` and run `./android-cross.sh`.

---

## Capabilities

| Area | Capability |
|---|---|
| **Image formats** | qcow2 create / expand / compact / clone; raw, VHD (dynamic), VMDK (sparse) read-write; format detected by magic, not extension |
| **Partition tables** | MBR / GPT read-write; role-based auto layout (ESP + MSR + system + recovery); add / remove / resize; attribute bits; whole-table export / import |
| **FAT32** | List, read, write, mkdir, rename, delete, volume label, timestamps, 8.3 short names |
| **NTFS** | List, read, write, recursive copy / move, delete, `du`, volume label, format (mkntfs) |
| **WIM / ESD** | List images, estimate expanded size, apply to a partition, extract a single file, **capture** (qcow2 → new WIM) |
| **ISO** | Browse ISO9660 / UDF, extract inner files (`install.wim` can be read straight from an ISO) |
| **Archives** | zip / 7z / tar / tgz read-write; RAR / CAB read-only; a single `copy` command covers extraction and packing |
| **Boot** | Write MBR/VBR boot code; lay down `bootmgr` / `bootmgfw.efi` / `boot.sdi` / fonts; generate BCD (BIOS and UEFI); detect 32-bit UEFI loaders; boot-chain self-check |
| **Recovery** | Install `winre.wim` into a recovery or system partition; write `ReAgent.xml` and recovery scripts |
| **Registry** | Offline read-write of the 5 standard hives (SYSTEM / SOFTWARE / SAM / SECURITY / DEFAULT) with transactional writes |

**Format support matrix**

| Format | Read | Write | Create |
|---|---|---|---|
| qcow2 | ✅ | ✅ | ✅ |
| raw (.img / .raw / .dd) | ✅ | ✅ | ✅ |
| VHD (vpc dynamic) | ✅ | ✅ | ⚠️ convert from raw, see below |
| VMDK (sparse) | ✅ | ✅ | ✅ |

> Creating a VHD is not yet supported (`img.create --format=vhd` returns `IMG-FORMAT-NOTSUP`).
> Workflow: `img.create --format=raw`, then `qemu-img convert -f raw -O vpc`.

**Design boundaries**

- Does not boot a VM, does not use the network, does not mount images;
- Images must be **powered off**: the tool bypasses filesystem drivers and edits metadata directly, so concurrent writers corrupt the image;
- No `fork/exec` on Android; everything runs in-process.

---

## Command line

```bash
imgcmd <command> [--key=value ...]
imgcmd --list                     # dump the command table (JSON)
imgcmd --help=<command>           # usage for one command
```

- Every argument is a string: `--img=/sdcard/a.qcow2`, `--recursive=true`
- Results go to stdout (one JSON line); logs go to stderr
- **Success**: `{"ok":true,"cmd":"img.info","version":1,"duration_ms":12,"data":{...}}`
- **Failure**: `{"ok":false,...,"err":{"rc":-4,"error_code":"IMG-OPEN-FAIL","at":"img.info","hint":"<description>","suggest":"<next step>"}}`

`error_code` is the stable contract; `hint` and `suggest` are human-readable text and **must not be parsed**.

### Unified path syntax

```
/path/a.qcow2!p3:/Users/a.txt    third used partition of a qcow2
/path/a.qcow2!lba165888:/a.txt   explicit LBA
/path/a.iso!:/sources            ISO root directory
/path/a.esd!1:/Windows           image 1 inside a WIM/ESD
/path/a.zip!:/dir1               directory inside an archive
host:/sdcard/x                   host path (absolute paths also accepted)
```

Container type is determined by **magic bytes**, never by extension.

### Typical workflow

```bash
# 1) Create a disk (GPT + ESP + MSR + Windows)
imgcmd img.create --out=/sdcard/win11.qcow2 --size-gb=64 --table=gpt

# 2) Apply the image
imgcmd wim.apply --img=/sdcard/win11.qcow2 --lba=<SYS> \
    --wim=/sdcard/win11.iso --index=1 --format=on

# 3) Lay down boot files (UEFI: bootmgfw.efi + bootx64.efi + fonts + boot.sdi + built-in BCD)
imgcmd boot.populate --img=/sdcard/win11.qcow2 --mode=uefi --sys-lba=<SYS> --esp-lba=<ESP>

# 4) Self-check
imgcmd boot.check --img=/sdcard/win11.qcow2 --mode=uefi
```

Full command table: `imgcmd --list` (76 commands).

---

## Using from C

```c
#include "img_cmd.h"

char err[256];
img_ctx *ctx = img_ctx_open("/sdcard/a.qcow2", true, false, err, sizeof err);
if (!ctx) { /* could not open; err holds a description */ }

img_args *a = img_args_new();
img_args_set(a, "lba", "2048");
img_result *r = img_result_new();
img_cmd_exec("part.scan", ctx, a, r);
/* read fields from r when r->rc == IMG_OK */

img_result_free(r); img_args_free(a); img_ctx_close(ctx);
```

`img_cmd_exec()` shares the same command table as the CLI: identical argument specs, validation, and error codes.

---

## Return codes

`img_result.rc` matches the process exit code (defined in `include/img_types.h`):

| rc | Name | Meaning |
|---|---|---|
| 0 | `IMG_OK` | Success |
| -1 | `IMG_EINVAL` | Invalid argument |
| -2 | `IMG_ENOMEM` | Out of memory |
| -3 | `IMG_EIO` | Low-level I/O failure |
| -4 | `IMG_ENOENT` | Not found |
| -5 | `IMG_ENOTSUP` | Not supported (e.g. zstd, encryption) |
| -6 | `IMG_EFORMAT` | Invalid format (magic / structure) |
| -7 | `IMG_ECORRUPT` | Corrupt data (checksum mismatch) |
| -8 | `IMG_EAGAIN` | Retry required / resource busy |
| -9 | `IMG_ECRC` | Checksum / CRC mismatch |
| -10 | `IMG_EFULL` | Out of space |
| -11 | `IMG_EEXISTS` | Already exists |
| -12 | `IMG_ESTATE` | Operation not allowed in current state |
| -13 | `IMG_ECANCELLED` | Cancelled |
| -14 | `IMG_ETRUNC` | Data truncated |
| -15 | `IMG_ERANGE` | Range out of bounds |
| -16 | `IMG_ETIMEDOUT` | I/O watchdog timeout |
| -17 | `IMG_ECYCLE` | Would create a cycle |
| -18 | `IMG_ESAMEFILE` | Source and destination are the same file |
| -19 | `IMG_ENAMETOOLONG` | File name exceeds filesystem limit |
| -20 | `IMG_EPARTIAL` | Partially completed |
| -21 | `IMG_EREFUSED` | Refused by policy |
| -22 | `IMG_EREADONLY` | Target is read-only (ISO / WIM / ESD) |
| -23 | `IMG_ENAMEINVALID` | Name rejected by Windows rules |
| -24 | `IMG_ESTALE` | Stale handle / index (archive changed) |

---

## Layout

```
include/            Public headers (img_*.h)
src/                Implementation: qcow2 / raw / VHD / VMDK / partition tables / FAT32 /
                    NTFS / WIM / ISO / archives / BCD / boot / recovery / registry
tools/              imgcmd, imgbc, imglayout, imgdump … (plus build-mkntfs-obj.sh)
tests/              Shell, Python, and C regression tests
reference/          Boot reference material (archive only; binaries not tracked)
legacy/             Early implementation, kept for reference, not built
third_party/unrar/  Bundled RAR decoder
LICENSES/           Full license texts
cmake/              Android NDK toolchain (android-arm64host.cmake)
android-cross.sh    Android cross-compile entry point
android-deps.sh     One-time Android dependency build
```

Docs: `ANDROID-BUILD.md`, `ARMHF-BUILD.md`, `DESIGN-OUTPUT.md`,
`PARTITION-POLICY.md`, `SPEC-NTFS-MOVE.md`, `WORKPLAN-HARDEN.md`, `THIRD_PARTY.md`.

---

## Engineering constraints

- **Images must be powered off**: concurrent writers corrupt metadata.
- **No silent degradation**: when an optional dependency is missing, the matching commands return `FEATURE-UNAVAILABLE` instead of silently succeeding.
- **Validate before writing**: file names are checked against NTFS/FAT rules, free space is verified before partition operations, hives are backed up before modification.
- **Transactional writes**: registry, BCD, and archive rewrites all use "temp file → verify → rename".
- **No network**: there are no network calls anywhere in the code.

---

## License

The default build (including `mkntfs`) is **GPL-2.0-or-later**.
A `-DIMG_WITH_NTFS=OFF` build contains only this project's own code plus the
LGPL / BSD / Zlib components listed below.

### This project

| Scope | License |
|---|---|
| All original code | **GPL-2.0-or-later** (an LGPL-2.1-or-later-compatible set when built with `-DIMG_WITH_NTFS=OFF`) |

### Third-party components linked into the artifact

| Component | License | Purpose | Text |
|---|---|---|---|
| [zlib](https://zlib.net) | Zlib | qcow2 compressed clusters | [Zlib.txt](LICENSES/Zlib.txt) |
| [libntfs-3g](https://github.com/tuxera/ntfs-3g) | LGPL-2.1-or-later | NTFS read/write | [LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| [ntfsprogs / mkntfs](https://github.com/tuxera/ntfs-3g/tree/master/ntfsprogs) | **GPL-2.0-or-later** | NTFS formatting (removable with `-DIMG_WITH_NTFS=OFF`) | [GPL-2.0.txt](LICENSES/GPL-2.0.txt) |
| [wimlib](https://wimlib.net) | LGPL-2.1-or-later | WIM / ESD | [LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| [libarchive](https://libarchive.org) | BSD-2-Clause / BSD-3-Clause | zip / 7z / tar | [BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt) |
| [hivex](https://github.com/libguestfs/hivex) | LGPL-2.1-or-later | Registry hives (BCD) | [LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| [UnRAR](https://www.rarlab.com/rar_add.htm) | UnRAR freeware license | RAR extraction only | [UnRAR.txt](LICENSES/UnRAR.txt) |
| [libxml2](https://gitlab.gnome.org/GNOME/libxml2) | MIT | hivex dependency | [MIT.txt](LICENSES/MIT.txt) |
| [libiconv](https://www.gnu.org/software/libiconv/) | LGPL-2.1-or-later | Charset conversion | [LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| [xz / liblzma](https://tukaani.org/xz/) | 0BSD (some files public domain) | libarchive dependency | [xz upstream](https://git.tukaani.org/?p=xz.git;a=blob_plain;f=COPYING;hb=HEAD) |
| Android NDK (sysroot) | Apache-2.0 + component licenses | Cross-compile target | [Apache-2.0.txt](LICENSES/Apache-2.0.txt) |

### Development / verification only (not shipped)

| Component | License | Purpose |
|---|---|---|
| CMake | BSD-3-Clause | Build |
| GCC / clang | GPL-3.0+ (with runtime exception) | Compilation |
| QEMU (`qemu-img check`) | GPL-2.0 | Independent image and format verification |
| gptfdisk (`sgdisk -v`) | GPL-2.0 | Independent GPT verification |
| util-linux (`fdisk`) | GPL-2.0+ | Independent MBR verification |
| mtools (`mdir`/`mcopy`) | GPL-2.0+ | Independent FAT32 verification |
| ntfs-3g tools (`ntfsls`/`ntfscp`) | GPL-2.0+ / LGPL-2.1+ | Independent NTFS verification |
| 7-Zip CLI | LGPL-2.1 | Archive cross-verification |
| Python 3 | PSF-2.0 | Test scripts |

### Compliance notes

- **LGPL obligations**: `libntfs-3g`, `wimlib`, `hivex`, and `libiconv` are statically linked into `libimgtool.so`.
  To satisfy the relinking requirement, this repository publishes the complete sources,
  build scripts, and dependency versions needed to regenerate the `.so`.
- **GPL obligations**: the default build includes `mkntfs` (GPL-2.0-or-later), so the whole
  artifact is distributed under GPL-2.0-or-later. Use `-DIMG_WITH_NTFS=OFF` for a non-GPL distribution.
- **UnRAR restriction**: used for decompression only; must not be used to develop a RAR-compatible
  compressor or to reverse-engineer the RAR compression algorithm.
- Per-component modification notes and copyright holders: [`THIRD_PARTY.md`](THIRD_PARTY.md).
