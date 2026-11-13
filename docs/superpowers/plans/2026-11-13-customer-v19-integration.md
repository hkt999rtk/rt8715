# Customer v19 Integration Plan

> Execute inline with superpowers:executing-plans; independent final review.

Goal: integrate ~/carplay_app_19.rar, verify native feedback/pacing, retire redundant audio wrappers, build a reproducible image in the main workspace.

Architecture: reuse the existing media-clock-drift worktree and preserve the tested diagnostic paths. Customer modules own PCM pacing and continuous feedback; SDK only observes. Preserve all other tested production behavior unless binary evidence proves a replacement.

Constraints: external Realtek GCC 6.4.1; sequential clean, LP -j1 -B, IS -j8. Preserve existing edits and raw vendor archives. Never bypass unknown-version patch guards; update only audited identities/relocations. No hardware validation claims.

## Tasks

- [x] Audit every changed member; verify source-equivalent feedback/pacer, actual native callsites, state layout, output path, Screen/Timer/Clock changes.
- [x] Install all 14 vendor libraries; replace old ABI/hash checks with audited v19 checks, retire StartAudio/SendAudio/FeedBack wrapper source/build/reporting paths, adapt only required diagnostic relocations.
- [x] Run focused patch/native-call regression checks including rejection of old/unknown vendor objects and redundant wrapper config.
- [x] Clean build, inspect final ELF for unique native module definitions and actual call targets, run independent final review, synchronize reviewed source to main workspace and build its default image and record hash.

Review focus: duplicate PCM control; stale derived archives; changed stream ABI; old relocation offsets; loss of shared HAL timing; production vs diagnostic build options. User request supplies integration authorization; proceed without additional approval.
