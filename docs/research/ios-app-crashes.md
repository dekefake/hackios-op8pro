# iOS app crashes at launch: MobileNotes and MobileSMS (2026-10-27)

Status: analysis only (no phone run). Symbolication done from the iOS 16.5 (20F66, iPhone10,3) dyld shared cache on disk.

## Observed (from the device run's kernel trace, exctrace=1)
- `usertrap: MobileNotes[179..208] exception 1 code 0x2 0x16ed73ff0: pc 0x1d0e3a664 lr 0x1de52da74 sp 0x16ed74010`, shared cache slide 0x49fe0000.
- `usertrap: MobileSMS[210] exception 1 code 0x2 0x16f597fe0: pc 0x1de52da00 lr 0x1de52da74 sp 0x16f597fd0`.
- EXC_BAD_ACCESS / KERN_PROTECTION_FAILURE, fault address 0x10 (SMS) or 0x20 (Notes) below/above sp: a stack guard page is hit.
- The kernel's frame walk after the fault lists `called from 0x1de52da74` over and over (about ten frames). Every frame has the same return address.
- `libhackios-ui: loaded in MobileNotes` is the last line before the crash. The status-bar line `answers by the screen's width` never appears in Notes or SMS. It does appear in SpringBoard and Preferences, which both load the same library.

## Symbolication (observed from the cache; unslid = runtime - 0x49fe0000)
| runtime | unslid | image + offset | function |
|---|---|---|---|
| 0x1d0e3a664 (Notes pc) | 0x186e5a664 | CoreFoundation + 0x7e664 | `_CFAllocatorAllocate`, exact entry |
| 0x1de52da00 (SMS pc) | 0x19454da00 | libsystem_malloc + 0x1aa00 | unexported, function start 0x19454d9fc, fault at its 2nd instruction |
| 0x1de52da74 (lr, both) | 0x19454da74 | libsystem_malloc + 0x1aa74 | same unexported function, the `blr x8` return site |

The unexported function (0x19454d9fc) is libmalloc's generic zone allocation path. Its calls go through the zone's malloc slot:
`ldr x8,[x21,#0x18]; blr x8` at 0x19454da68-da70. The entry that tail-calls it is 0x19453817c, reached from `_malloc` (0x1945344d0). That entry uses the default zone pointer at `*(0x1e1ea5388)` when the caller passes the placeholder 0x1dd3cc000.

## What the recursion is (observed structure, inferred cause)
- SMS: zone+0x18 points back into 0x19454d9fc itself. The chain is 9fc -> blr [zone+0x18] -> 9fc, and every frame returns to da74. Stack grows by 0x50 per level until the guard page.
- Notes: zone+0x18 is `_CFAllocatorAllocate`. Its frames are the same loop: malloc dispatch -> CFAllocatorAllocate -> malloc.
- Either way the malloc zone's malloc slot holds a function that calls back into malloc. Nothing in userland/src touches malloc zones (`malloc_zone_register`, `malloc_default_zone`, stack logger, etc. are not referenced). So the slot is corrupted or re-pointed at runtime, not hooked by our code.

## Which code runs in these processes that does not run in the working ones
- build.sh INJECT: `/hackios/lib/libhackios-ui.dylib SpringBoard Preferences MobileSafari MobileNotes Maps MobileSMS`. Camera was removed from this list on 2026-10-27 for the same signature: pc in CoreFoundation, called from libsystem_malloc, runaway recursion (build.sh comment).
- src/inject_ui.c: constructor calls `_dyld_register_func_for_add_image(image_added)`. `image_added` calls `try_replace()` on every image add. Until UIKit's `_UIStatusBarVisualProvider_Split1125` exists, it returns early. Once it exists it runs `objc_copyClassList`, `class_getClassMethod` and `method_setImplementation` from inside dyld's add-image callback.
- That is the only new code these apps run, and it calls objc and malloc from inside a dyld notification. This is the most likely trigger (inferred, not proven).
- AGENTS.md mentions `userland/inject.conf`. The file does not exist. The injection list is `INJECT` in userland/build.sh.

## Keyboard `.?123` (not resolved)
- Keyboard service: `com.apple.TextInput.kbd` is spawned (log, 20:40:53). Its `mobileassetd.v2` lookups fail (mobileassetd is on the DENY list in mkjobs.py). Those asset lookups are a candidate, not a confirmed cause.
- Layout plists: `TextInput.framework/Keyboard-default.plist` and `UIKitCore.framework/Keyboard-default.plist` hold accent popups (Roman-Accent-*), not the numeric plane. The `.?123` label is not a plain string in the cache (`grep -F '.?123'` over all cache files found nothing). The layout switch lives in UIKit's compiled layouts, which were not located.
- `UIKitCore.framework/XPCServices/com.apple.UIKit.KeyboardManagement.xpc` exists on disk. It is not in mkjobs.py, so it is not started by this image's launchd setup (inferred from the absence of any mention; not checked against the job list).
- The plist comments in userland/var/mobile/Library/Preferences/ (keyboard continuous-path film, empty Offline Dictation Status) describe keyboard hangs that were already known. They are not the `.?123` cause.

## Smallest things to try (not run)
1. Remove MobileNotes and MobileSMS from the libhackios-ui list in build.sh (the Camera workaround). If they launch, the library is the trigger.
2. If so, defer `try_replace` out of the add-image callback (for example to the first `UIApplicationMain` run loop turn), or only install it in SpringBoard and Preferences, where it works.
3. For `.?123`: check whether `com.apple.UIKit.KeyboardManagement` and `com.apple.TextInput.kbd` answer when the keyboard appears; capture the lookup failures for the keyboard at the moment the key is pressed.

## Log note
`logs/res/camera-live.txt` was zero bytes at 21:47 (overwritten by another process). Crash lines above were read from it before that. `logs/res/camera-live.txt.final` (20:40:04-20:47:13) was used for the frame walk and the other apps' status. The 20:37-20:38 window asked for is not in either file.

## Tools (scratch, not in repo)
- Cache reader and Mach-O views: scratchpad `cache.py`, `malloc.macho`, `malloc.dis` (llvm-objdump from Xcode CLT). Derived from the cache; not committed.
