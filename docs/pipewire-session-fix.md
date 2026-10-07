# PipeWire capture session crash fix

## Outcome and scope

Resolve the locally observed OBS 32.2.2 SIGSEGV when selecting a PipeWire
screen/window capture. Build only linux-pipewire against the installed libobs
32.2.2. Preserve the user's scenes, profiles, and other plugins. Keep the
original plugin available for rollback. Do not publish an upstream report.

## Evidence and assumption

The core for PID 100864 shows a null session_handle passed to the portal Start
request. Two earlier crashes have the same stack. A new capture was initialized
twice before the crash. The source's reload callback clears session_handle but
does not cancel the old requests before replacing cancellable. A stale response
from an earlier session was the inferred trigger. The regression harness now
reproduces the identical GLib SIGSEGV at address 0x30 using the original tag,
confirming that this request lifecycle can cause the observed failure.

## Requirements

- Cancel and release the previous cancellable before clearing capture state or
  starting a replacement session; do the same before destroying the source.
- Keep portal response userdata alive for the D-Bus subscription lifetime,
  including queued responses after cancellation.
- Disconnect cancellation before invoking a response callback so a callback
  that cancels/reloads does not free its own callback state twice.
- Release subscription-owned cancellable references.
- Reject missing session handles before constructing portal requests.
- Reject malformed successful CreateSession responses without crashing.

## Acceptance and verification

An isolated D-Bus regression harness must exercise delayed replies after reload,
repeated reload, cancellation before a response, cancellation from a response
callback, source destruction with queued replies, malformed session responses,
and the normal CreateSession -> SelectSources -> Start flow. The original source
must fail the relevant regression and the patched source must pass. Run with
AddressSanitizer/UndefinedBehaviorSanitizer where supported. Build the complete
replacement plugin, check dynamic linking, and verify that OBS loads it.
Actual monitor/window capture requires a selection in the desktop picker;
report any limits on interactive verification explicitly.

## Local deployment

Repackage the cached Arch obs-studio 32.2.2-1 as 32.2.2-1.1 with the rebuilt
plugin. A SHA-256/mode/symlink comparison of all 1,888 payload entries must show
only usr/lib/obs-plugins/linux-pipewire.so changed. Install through pacman;
retain a copy of the original package in local-fix/rollback. Future newer Arch
package releases can replace this local build normally.

## Verified result

- The original-source executable reproduced the same SIGSEGV at address 0x30.
- The patched private D-Bus suite passed with AddressSanitizer,
  UndefinedBehaviorSanitizer, and leak detection enabled.
- The installed package passed `pacman -Qkk`: 1,990 entries, zero altered files.
- OBS loaded the installed plugin and logged four active capture streams.
- The user tested capture selection and re-selection and confirmed:
  "Capture works without crashing."
- The live OBS session subsequently shut down cleanly with zero reported
  memory leaks.

All acceptance criteria are met for the targeted crash. The separate earlier
Hyprland portal crashes were not modified by this fix.
