# Building Linux QuickSync (QSV) locally against a build-deps clone

How to build Sunshine's Intel QuickSync support on Linux from a local `build-deps`
checkout, instead of the prebuilt FFmpeg tarball that CMake normally downloads from a
`LizardByte/build-deps` GitHub release.

You need this whenever you are changing anything in `third-party/build-deps`: the
submodule pointer only selects a *published release tarball*, so an unreleased build-deps
change is invisible to a normal Sunshine build.

> **Target distro: Arch Linux.** Every package name and path below is Arch's. The build
> steps themselves are distro-neutral; only the prerequisites and the runtime packages in
> step 3 need translating for another distro.

## Layout

Two sibling clones:

```
~/Projects/
├── Sunshine/      # this repo
└── build-deps/    # clone of https://github.com/LizardByte/build-deps
```

## Prerequisites

`build-deps` compiles FFmpeg, x264, x265 and SVT-AV1 from source, so it needs the full
toolchain listed in `build-deps/README.md`. **`nasm` is mandatory** — x264, x265, SVT-AV1
and FFmpeg's x86 assembly all fail without it.

```bash
sudo pacman -S --needed autoconf automake cmake git libtool make meson nasm ninja pkgconf
```

Sunshine itself needs its own dependencies. The authoritative list is `add_arch_deps()` in
`scripts/linux_build.sh`; `scripts/linux_build.sh --help` will install them for you. See
also `docs/building.md`.

### The compiler is pinned, and Arch's default `gcc` is not it

`scripts/linux_build.sh` pins Sunshine to a specific GCC major version and exports
`CC=gcc-${gcc_version}` / `CXX=g++-${gcc_version}`. On Arch that is currently **15**:

```bash
sudo pacman -S --needed gcc15 gcc15-libs
```

Arch's unversioned `gcc` package has moved on to 16, so a plain `cmake` invocation picks
up a compiler the project does not pin. Match the script when configuring by hand — every
`cmake -B build` below assumes it:

```bash
export CC=gcc-15 CXX=g++-15
```

Check which version the script is actually pinned to rather than trusting this paragraph,
since it moves as Arch drops old `gccNN` packages:

```bash
grep -A6 '"Arch Linux"' scripts/linux_build.sh | grep gcc_version
pacman -Ss '^gcc[0-9]+$'     # which versioned compilers Arch still ships
```

---

## Step 1 — build FFmpeg with libvpl

```bash
cd ~/Projects/build-deps
git submodule update --init --recursive

mkdir -p build/dist
cmake -B build -S . -G "Unix Makefiles" \
  -DBUILD_ALL=OFF \
  -DBUILD_FFMPEG=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/dist" \
  -DPARALLEL_BUILDS="$(nproc)"

make -C build --jobs="$(nproc)"
make -C build install
```

`BUILD_FFMPEG_LIBVPL` defaults to `ON` and is auto-enabled only on Linux x86_64 — the
dispatcher is built from source there, whereas Windows resolves it from the MSYS2
`onevpl` package. Pass `-DBUILD_FFMPEG_LIBVPL=OFF` to reproduce the pre-change behaviour.

Confirm the configure line before the long build starts; it should contain:

```
--enable-libvpl --enable-encoder=h264_qsv,hevc_qsv,av1_qsv,mpeg2_qsv ... --extra-libs='-lstdc++'
```

### Verify the result

```bash
cd ~/Projects/build-deps

# 1. the dispatcher was built and shipped into the tarball tree
ls -l build/libvpl/lib/libvpl.a build/dist/ffmpeg/lib/libvpl.a

# 2. its .pc carries the C++ runtime (see Troubleshooting) -- must print "-lstdc++"
PKG_CONFIG_PATH=$PWD/build/libvpl/lib/pkgconfig pkg-config --static --libs vpl

# 3. FFmpeg enabled QSV
grep -E '^#define CONFIG_(LIBVPL|QSV|VAAPI) ' build/FFmpeg/FFmpeg/config.h
grep -E 'QSV_ENCODER' build/FFmpeg/FFmpeg/config_components.h

# 4. the encoders are really in the archive
nm --defined-only build/dist/ffmpeg/lib/libavcodec.a 2>/dev/null | grep -E 'ff_(h264|hevc|av1)_qsv_encoder'
nm --defined-only build/dist/ffmpeg/lib/libavutil.a  2>/dev/null | grep ff_hwcontext_type_qsv
```

Expected: `CONFIG_LIBVPL 1`, `CONFIG_QSV 1`, `CONFIG_VAAPI 1`,
`CONFIG_H264_QSV_ENCODER 1` (and hevc/av1/mpeg2), plus `ff_h264_qsv_encoder` and
`ff_hwcontext_type_qsv` defined.

> Arch ships a system `libvpl` package, and having it installed is harmless — but it is
> **not** what gets linked. build-deps builds its own static dispatcher, and step 2 links
> that one. Do not "fix" a missing-QSV problem by installing system `libvpl`.

### `lib` vs `lib64` — the Arch trap

SVT-AV1 includes GNUInstallDirs, which resolves `CMAKE_INSTALL_LIBDIR` to **`lib64`** on
64-bit non-Debian distros — Arch included. Every other component installs to `lib/`. So on
Arch the prepared tree comes out split:

```bash
ls build/dist/ffmpeg/lib/libSvtAv1Enc.a build/dist/ffmpeg/lib64/libSvtAv1Enc.a 2>&1
```

A `lib64/` copy is the symptom. This never reproduces in CI, where the release tarballs are
built on Ubuntu and everything lands in `lib/`.

Both sides are now fixed — `cmake/ffmpeg/svt_av1.cmake` in build-deps pins
`CMAKE_INSTALL_LIBDIR` to `lib` (as `libvpl.cmake` already did), and Sunshine's
`cmake/dependencies/ffmpeg.cmake` probes `lib/` then `lib64/`. If you are on a build-deps
checkout predating that pin, either rebuild after adding it or symlink the archive:

```bash
ln -sf ../lib64/libSvtAv1Enc.a build/dist/ffmpeg/lib/libSvtAv1Enc.a
```

Left unresolved, this surfaces only at the very end of the Sunshine build — see
Troubleshooting.

> The FFmpeg build tree is `build/FFmpeg/FFmpeg/`, **not** `build/generated-src/`.
> `.github/workflows/ci.yml` still cats the old `generated-src` path, so the FFmpeg
> `config.log` is currently missing from CI logs.

---

## Step 2 — build Sunshine against that tree

```bash
cd ~/Projects/Sunshine
git submodule update --init --recursive

export CC=gcc-15 CXX=g++-15

cmake -B build -S . -G "Unix Makefiles" \
  -DBUILD_DOCS=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DFFMPEG_PREPARED_BINARIES="$HOME/Projects/build-deps/build/dist/ffmpeg"

cmake --build build -j4
```

`FFMPEG_PREPARED_BINARIES` short-circuits the release-tarball download entirely — no
network, no tag resolution. **Use an absolute path.** CMake should print:

```
-- Using user-specified FFmpeg binaries at /home/<you>/Projects/build-deps/build/dist/ffmpeg
```

Confirm the dispatcher reached the link line:

```bash
grep -o 'libvpl\.a' build/CMakeFiles/sunshine.dir/link.txt
```

`cmake/dependencies/ffmpeg.cmake` adds `libvpl.a` only `if(EXISTS ...)`, so an older
build-deps tarball without it still links — it just yields no QSV encoders.

> Use `-j4` rather than `-j$(nproc)` on machines with limited RAM. A Debug build of
> Sunshine at full parallelism gets the compiler OOM-killed
> (`c++: fatal error: Killed signal terminated program cc1plus`).

> `CC`/`CXX` are read at configure time only. If you set them after configuring, delete
> `build/` and reconfigure — otherwise the cache keeps Arch's default compiler.

---

## Step 3 — verify at runtime

```bash
mkdir -p /tmp/sun && cd /tmp/sun
printf 'min_log_level = debug\nport = 57989\n' > sunshine.conf
~/Projects/Sunshine/build/sunshine /tmp/sun/sunshine.conf 2>&1 | tee out.log
```

Look for, in order:

1. The FFmpeg build actually in use — this is the fastest way to tell whether you are
   running against your local tarball or a downloaded one:
   ```
   Debug: FFmpeg configuration: ... --enable-libvpl --enable-encoder='h264_qsv,hevc_qsv,av1_qsv,mpeg2_qsv' ...
   ```
2. The encoder probe reaching quicksync:
   ```
   Info: Trying encoder [quicksync]
   Info: Creating encoder [h264_qsv]
   ```
3. On Intel hardware with a VPL runtime installed: `Found H.264 encoder: h264_qsv [quicksync]`.

On non-Intel hardware you will instead see `Encoder [quicksync] failed` followed by
`Trying encoder [vaapi]`. That fall-through is the expected, correct behaviour — not a
bug — since quicksync is probed ahead of vaapi on Linux.

### Runtime requirement

Sunshine statically links only the oneVPL *dispatcher*; it `dlopen`s an implementation at
runtime. Without one, QSV initialisation fails and Sunshine silently falls back to VA-API.
Building QSV in is therefore not enough — the runtime is a separate, easily-forgotten
install.

Pick by GPU generation (all in `extra`):

```bash
# Tiger Lake and newer -- provides libmfx-gen.so.1.2
sudo pacman -S --needed vpl-gpu-rt

# Broadwell .. Rocket Lake -- the legacy MediaSDK runtime
sudo pacman -S --needed intel-media-sdk libmfx

# the VA-API driver, needed either way; QSV is derived from the VA device
sudo pacman -S --needed intel-media-driver libva
```

`vpl-gpu-rt` and `intel-media-sdk` are the *implementations*. `libmfx` is only the legacy
dispatcher — installing it alone gives you nothing to dispatch to, which is the usual
reason QSV stays invisible after a "successful" install.

Check what the dispatcher can actually find:

```bash
ls -l /usr/lib/libmfx-gen.so.1.2     # vpl-gpu-rt
ls -l /usr/lib/libmfxhw64.so*        # intel-media-sdk
vainfo                               # must list the Intel driver and H.264/HEVC entrypoints
```

If neither library is present, no amount of rebuilding will produce QSV. To confirm which
package owns a given file on this machine:

```bash
sudo pacman -Fy            # once, to sync the file database
pacman -F libmfx-gen.so.1.2
```

---

## Step 4 — tests

```bash
cmake -B build -S . -DBUILD_TESTS=ON
cmake --build build --target test_sunshine -j4
./build/tests/test_sunshine --gtest_filter='*Qsv*:*Encoder*:*ConfigConsistency*'
```

`EncoderTest.ValidateEncoder/quicksync` reports **SKIPPED** when no usable Intel device is
present; that is expected off Intel hardware, not a failure.

`SystemTrayVisualTest` fails without a screenshot tool — on Arch install `gnome-screenshot`,
or `imagemagick` for X11 `import`, and make sure a display is attached. It is unrelated to
encoder work.

---

## Troubleshooting

**`ERROR: libvpl >= 2.6 not found` during FFmpeg configure**

The most likely failure, and the message is misleading. libvpl's `vpl.pc` does not list
`-lstdc++` even though the dispatcher is C++, so FFmpeg's check — a **C** link test run
with `--pkg-config-flags=--static` — fails on C++ runtime symbols. Confirm with:

```bash
grep 'operator new\|__gxx_personality' ~/Projects/build-deps/build/FFmpeg/FFmpeg/ffbuild/config.log
```

The fix is already in `cmake/ffmpeg/libvpl.cmake` (`-DCXX_LIB=-lstdc++`, which feeds
libvpl's `vpl.pc.in`) plus `--extra-libs='-lstdc++'` in `cmake/ffmpeg/ffmpeg.cmake`. If
you see this, one of those two did not take effect.

**`No CMAKE_ASM_NASM_COMPILER could be found`** — `sudo pacman -S --needed nasm`.

For a libvpl-only smoke test without nasm you can skip the assembly-dependent components,
but the resulting tree is **not** usable for a full Sunshine build (no x264/x265/SVT-AV1):

```bash
cmake -B build -S . -G "Unix Makefiles" -DBUILD_ALL=OFF -DBUILD_FFMPEG=ON \
  -DBUILD_FFMPEG_SVT_AV1=OFF -DBUILD_FFMPEG_X264=OFF -DBUILD_FFMPEG_X265=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/dist"
make -C build libvpl --jobs="$(nproc)"
```

**`undefined reference to svt_av1_enc_*` when linking `sunshine`**

The SVT-AV1 archive is not on the link line. `libavcodec.a` was built with libsvtav1
enabled, so it hard-references those symbols; if `libSvtAv1Enc.a` is missing, the failure
lands at the final link rather than at configure time. On Arch the cause is almost always
the `lib64` split described in step 1. Check where it actually is, and what CMake resolved:

```bash
ls ~/Projects/build-deps/build/dist/ffmpeg/lib*/libSvtAv1Enc.a
tr ' ' '\n' < build/CMakeFiles/sunshine.dir/link.txt | grep SvtAv1
```

`ffmpeg.cmake` resolves these at **configure** time, so after moving or symlinking the
archive you must re-run `cmake -B build -S .` — `cmake --build` alone will not notice.
Configure now also prints `Optional FFmpeg component not found, skipping: <lib>` for each
component it could not resolve; that line appearing for `libSvtAv1Enc.a` is the early
warning for this exact link failure.

**Sunshine links but QSV never appears** — check `avcodec_configuration()` in the debug
log (step 3.1). If it shows a `/home/runner/work/...` prefix you are still running against
a downloaded CI tarball, so `FFMPEG_PREPARED_BINARIES` did not apply; delete `build/` and
reconfigure. If the configuration string *is* yours, the build is fine and the missing
piece is the runtime — go back to step 3's runtime requirement.

**Sunshine fails to compile with errors from system headers** — you are probably on Arch's
default `gcc` (16) rather than the pinned `gcc-15`. Confirm what the cache picked up:

```bash
grep -E 'CMAKE_(C|CXX)_COMPILER:' build/CMakeCache.txt
```

Delete `build/`, `export CC=gcc-15 CXX=g++-15`, and reconfigure.

---

## Promoting the change

`cmake/dependencies/ffmpeg.cmake` resolves a GitHub **release tarball** from the
`third-party/build-deps` submodule tag, so the submodule pointer cannot be bumped until
the build-deps change is merged upstream *and* a release is tagged. Until then,
`FFMPEG_PREPARED_BINARIES` is the only way to build against it.

Afterwards:

```bash
cd ~/Projects/Sunshine
git -C third-party/build-deps fetch --tags
git -C third-party/build-deps checkout <new-tag>
rm -rf build
cmake -B build -S .      # drop -DFFMPEG_PREPARED_BINARIES
```

CMake should then print `Using FFmpeg from build-deps tag: <new-tag>`, and
`build/_deps/ffmpeg/lib/libvpl.a` should exist.
