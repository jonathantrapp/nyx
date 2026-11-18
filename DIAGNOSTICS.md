# 0.3.8: diagnose the overnight panic before tuning LittleFS

This release is an investigation, not a claimed root-cause fix. Unit A's 0.3.7 test
had a panic after about 7.2 hours; no saved backtrace was available from that event.
Radio scheduling, logging modes, NVS values, OLED and filesystem tuning stay unchanged.

## What the reported breadcrumbs establish

- `crumb.loop=file_close` says the loop task entered the scoped `f.close()` call in
  `loggerFlush()` and had not left that scope when the retained record was captured.
  This narrows the live operation. It does not identify the faulting instruction or
  prove that the loop task caused the panic; a different task/core could have panicked.
- Flush start 25992225 to close entry 25992239 is 14 ms before close. The previous
  completed flush at 25987271 was about five seconds earlier. The pending flush's
  duration/completion cannot be reconstructed from these entry markers alone.
- `wifi_dwell` at 25992265 is a normal radio wait stage, timestamped after close
  entry. It shows that task reached a normal stage; it does not rule out a later
  radio/BLE fault, scheduler problem, shared-memory corruption or another task's panic.
- The free-space snapshot was 3821568 bytes. With intact state, this is well above
  the roughly 1 MiB plus pending-buffer rotation trigger, so capacity rotation should
  have returned without deleting anything. `stats rotated=0` is consistent with that.
- Normal size rollover is checked after `loggerFlush()` returns. There is no special
  LittleFS behavior added at 950 KiB or 1 MiB by our code. However a final old-session
  flush inside `loggerNewSession()` also uses this close scope, so the breadcrumb alone
  cannot exclude every rollover caller. The next crash's RTC flush-context record
  identifies service/new_session/mark/meta/dump and the pre-write file/buffer sizes.

The new context survives warm watchdog/panic resets with a commit marker/checksum.
It is not a fault-PC capture. Flash coredumps supply that independently.

## Suspects and how to distinguish them

1. **Loop stack exhaustion.** The installed core defaults to 8192 bytes, enables stack
   canary checking, and provides `SET_LOOP_TASK_STACK_SIZE`. main.cpp now requests 16384
   bytes. `stats` and ten-minute META report `loop_stack_bytes` and the actual loop task's
   `loop_stack_hwm_bytes`. This SDK's Xtensa StackType_t is uint8_t; the watermark is
   bytes. It is the smallest untouched stack area seen so far, not current free stack.
   A tiny watermark or decoded stack-canary/overflow panic supports this suspect. A
   generous watermark is useful evidence but does not guarantee all future paths fit.
2. **Heap corruption / bad pointer / out-of-bounds access.** The source audit found no
   obvious logger/CSV overrun. We now log internal-heap free/minimum/largest block and
   PSRAM free bytes. `heap-check` invokes the SDK's heap-integrity check at a requested
   checkpoint. An OK result is not proof that no earlier/later buffer overwrite exists.
   Decode the next crash for the exception, PC, EXCVADDR where applicable, task and
   stack. A heap assertion, invalid free or invalid-access fault can separate this from
   stack overflow. No full heap-poisoning SDK rebuild is attempted in this release.
3. **LittleFS/driver close/commit behavior.** Retain the full task backtraces. A specific
   LittleFS assertion or driver failure is actionable; an innocent close frame during
   another task's panic is not. Record the new flush context and phase timings. Keep
   the same filesystem image/occupancy while comparing 0.3.8, rather than formatting
   away the conditions under which the crash occurred.

## Bounds and lifetime audit

| Path | Audit result |
|---|---|
| `gPath[24]`, `next[24]` | Largest uint32 session path is 16 characters plus NUL, fitting both. gPath is copied only after the new file header has been written/closed. The old buffer is flushed before switching paths. |
| `gSt.session[16]` | `s` plus ten decimal digits plus NUL fits. Numeric overflow after 2^32 sessions is unrelated to a seven-hour run and is not silently claimed fixed. |
| `gDumpPath[24]` | Bounded copy; valid firmware session paths fit. Pins the export during pre-dump rotation. |
| META pieces | Each local piece is 33 bytes and holds at most 32 characters plus NUL. The source offset is bounded by the original strlen. |
| Clock / mode / rotation notes | Every snprintf/strlcpy uses its destination size. The UTC copy is capped at 39 characters; notes may split into META pieces rather than overrun. |
| Raw CSV | Student writer copies bounded, terminated CaptureRow fields into Strings and writes synchronously. SSIDs/extra quote embedded quotes and replace control characters. No dangling String pointer is stored in gBuf. |
| SUMMARY CSV | Numeric values are streamed to Print; no giant fixed extra buffer. The native round-trip checks ten columns, blank device fields and full-width counters. |
| `RamBuffer::write` | Writes copy immediately to owned RAM. Overflow rolls the row back to its saved start. The addition check is hardened to `n > cap - len`, preventing size_t wrap. Native tests cover exact fill, overflow, rewind, SIZE_MAX and zero-length writes. |
| Rollover / manifest | The old buffer is flushed before gPath changes, current/export paths are excluded from rotation, and path values are copied before vector erase. Protection state is copied when closing a session. |
| Concurrency | Logger/CSV/path/manifest code runs on the loop task. Radio callbacks queue CaptureRows; they do not mutate gBuf or gPath. This audit does not prove the underlying SDK or PSRAM is corruption-free. |

## Flush growth: distinguish maximum bookkeeping from actual slowdown

`flush_max_ms` was intentionally lifetime-since-boot. It cannot decrease, including
when session files roll. A rising maximum alone does not show that every flush got
slower. Higher fresh maxima after reboot at the same occupancy are consistent with
a filesystem-size effect, but this observation alone does not identify the phase.

Our full timer includes ensureSpace, open, write, size query, close and updateSpace.
In the installed Arduino LittleFS wrapper, both totalBytes() and usedBytes() call
esp_littlefs_info with non-null total/used outputs. In the bundled esp_littlefs 1.22.3,
the used-space calculation calls lfs_fs_size, which traverses filesystem allocation
information. We currently ask twice per flush. This is a concrete possible source of
occupancy-related latency outside the close operation, even across session boundaries.
[Bundled-version driver source](https://github.com/joltwallet/esp_littlefs/blob/v1.22.3/src/esp_littlefs.c)

New stats report the last full-call duration plus open/write/close/space-query durations.
Ten-minute META also reports:

```text
flush_last_ms
flush_win_max_ms
close_win_max_ms
space_win_max_ms
loop_stack_bytes
loop_stack_hwm_bytes
heap_int_free
heap_int_min
heap_int_largest
heap_psram_free
```

Window maxima reset after each ten-minute report; lifetime flush_max_ms remains unchanged.
Each phase's window maximum can come from a different call, so do not sum window maxima
as if they described one flush. The report's own write begins the next window.

580 ms is well below 15 seconds. It does not establish a worst-case bound at higher
occupancy/fragmentation, nor explain a non-watchdog panic by itself. Compare research
and car with similar existing filesystem usage: car's lower append rate does not make
a still-full filesystem empty or eliminate scheduled space queries.

### Next performance change, based on the phase data

- If space-query time dominates: use one esp_littlefs_info call for both outputs, cache
  immutable total capacity, and sample used space less often while preserving rotation
  safety margins. This avoids redundant traversal without changing the five-second
  data-loss window or keeping a writable file open indefinitely.
- If close/write time grows with occupancy: test earlier retention, for example starting
  around 3 MiB free and targeting 4 MiB, against the current image. This sacrifices some
  history for more free allocation space; it is a proposed follow-up, not applied here.
- The installed LittleFS.begin arguments do not expose cache/lookahead sizing; its
  maxOpenFiles argument is explicitly ignored. The SDK has 512-byte cache and 128-byte
  lookahead compiled in. Changing app preprocessor defines does not rebuild that SDK
  library. A custom esp_littlefs/SDK build would be a larger, separately tested change.

## Crash capture choice

The exact installed qio_opi SDK enables flash coredumps, ELF format, CRC32 checking,
64 task slots and a separate 1792-byte coredump stack. Arduino also exposes a panic
hook with PC/backtrace but no EXCVADDR in its public structure. We choose full flash
coredumps to retain saved registers and task snapshots, and avoid extra custom panic
code touching heap/filesystem/RTC during the fault.

The 256 KiB coredump partition is carved from unused app capacity. LittleFS offset
0x310000 and size 0xCF0000 are unchanged, as are NVS and PHY. App capacity becomes
0x2C0000 bytes (2.75 MiB), still comfortably larger than this firmware. No filesystem
shrink, move or format is needed. See [capture/decode steps](CORE_DUMP.md).

The next useful evidence is a decoded real crash plus the preceding ten-minute stack,
heap and timing rows. A successful intentional panic test verifies the capture route,
not the root cause or overnight reliability.
