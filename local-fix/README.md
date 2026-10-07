# Local OBS PipeWire session fix

Based on OBS tag `32.2.2`, commit
`ba2f32bdf791005443988a4955e963663e16b1ed`.

The source changes cancel pending portal requests before a capture session is
replaced or destroyed, defer subscription userdata cleanup until D-Bus releases
it, disconnect cancellation before invoking a response callback, and validate
session handles before constructing requests.

## Build and test

Run from the repository root with the installed Arch libobs development headers:

```sh
cmake -S local-fix -B local-fix/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build local-fix/build
ulimit -c 0
ctest --test-dir local-fix/build --output-on-failure
```

The suite creates a private D-Bus session, not the desktop's session. It covers
normal negotiation with object-path and legacy string handles; successful stale
selection replies after reload; repeated reloads with delayed creation replies;
source deletion with pending and already queued replies; cancellation inside a
response callback; reference release; already cancelled subscriptions; and
malformed/missing handles. AddressSanitizer and UndefinedBehaviorSanitizer are
enabled for the harness; the installed plugin is not sanitizer-instrumented.

The comparison executable uses the original tagged source:

```sh
ulimit -c 0
ASAN_OPTIONS=detect_leaks=0 dbus-run-session -- local-fix/build/test-session-original
```

This is expected to fail. It reproduces the original SIGSEGV at address `0x30`
in `g_variant_builder_add_value`, through `start` and
`on_select_source_response_received_cb`. The patched suite passes, including
with leak detection enabled.

## Installed package and rollback

Installed locally as `obs-studio 32.2.2-1.1`. The package payload is the cached
Arch `32.2.2-1` build with only `usr/lib/obs-plugins/linux-pipewire.so` replaced.
SHA-256, mode, and symlink comparison verified that the other 1,887 entries are
unchanged. `pacman -Qkk obs-studio` reports zero altered files. The rebuilt
plugin's SHA-256 is:

```
79ef0fb7c19361eb28485a7a37f9744ab9011e7d1547e8e32120d7d4551a45b6
```

Package: `local-fix/package/obs-studio-32.2.2-1.1-x86_64.pkg.tar.zst`.

To roll back, close OBS and run from the repository root:

```sh
sudo pacman -U local-fix/rollback/obs-studio-32.2.2-1-x86_64.pkg.tar.zst
```

The original archive is retained in that directory. Future newer Arch OBS
packages replace this local patch normally; no package version is pinned.

To repackage the existing built plugin, the two checksum-verified sources must
be present in `local-fix/package`: the original Arch archive and the built
`linux-pipewire.so`. `PKGBUILD` documents exactly how they are combined.

## Live verification

OBS was launched after installation. Its process mapped the installed plugin
(inode 1425754), and the application log showed four screen-capture streams
entering the `streaming` state. The private D-Bus test deterministically verifies
the previously crashing reload timing. The user also tested screen/window
selection and re-selection and confirmed capture works without crashing.
The live session subsequently shut down cleanly with zero reported memory leaks.
