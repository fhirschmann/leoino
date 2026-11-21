# Stability audit — November 2026

Scope: the maximum-volume/back-off crash, task-shared logging, volume LUTs, metadata and
cover parsing, LED DMA/RMT ownership, SD upload lifetime/error paths, and diagnostics.

## Findings and fixes

| Area | Evidence | Fix / regression check |
| --- | --- | --- |
| Log ring | Six concurrent writers plus snapshots reproduce index 2048 outside the 2048-byte buffer in the pinned library under UBSan. | Shared static mutex; 60,000 messages and concurrent snapshots in `test_log_concurrency.py`. |
| Volume LUT | Regular 0…1 inputs are bounded; infinity reaches an undefined float-to-int conversion in the previous function. No evidence that normal knob input supplies infinity. | Clamp before conversion; NaN maps to minimum. Both curves, all 256 setting values, 2.56M ramp points and 256k maximum/back cycles tested. |
| ID3 title | Exact old parsing block with a truncated `Title/` string produces an ASan heap-buffer-overflow. | Find the real delimiter rather than adding a fixed offset; short labels, ID3v2.2 and Vorbis tested. |
| Cover segments | Empty/odd image vectors were indexed without validation; short reads and failed seeks/writes were ignored. | Validate paired ranges, bounded reads and writes; publish cache only on success. 8,193 segment splits produce byte-identical output; malformed ranges and I/O failures rejected. The suspected `fLaC` terminator bug was disproved: the original string literal was terminated. |
| LED SPI/RMT | With a timeout injected, the old driver overwrites/requeues a pending transfer; the same host assertion fails against the old source and passes after the fix. | Preserve DMA buffer/transaction ownership until success, retain dirty frames, retry failed enqueue; both backends tested. |
| Upload buffers | Shared global buffers could be reset by another request or freed by a disconnect while the worker was writing. Reusing a reduced allocation reset its advertised capacity to the larger default. | Single request owner; no disconnect frees; retain aborted worker buffers until idle; reallocate before resetting capacity. Tests cover competing/unauthenticated requests and small→large allocation recovery. |
| Upload errors | Worker/path/semaphore creation unchecked; producer only split a callback into two chunks; task notifications could target deleted handles; partial-write retries duplicated bytes; zero-ms transfers divided by zero; task deletion skipped File destruction. | Checked startup, arbitrary bounded chunk splitting, atomic completion flags, tail-only retries, guarded elapsed time, normal C++ scope cleanup before task deletion. Real callback and worker functions run on host threads under ASan/UBSan. |
| Task diagnostics | Fixed 2048-byte unchecked formatter buffer on command path; `/stats` relied on estimated text capacity. | Bounded `uxTaskGetSystemState` snapshot, checked allocation, direct output and zero-runtime guard. |

## Reproduction

```sh
python3 tools/test_stability.py
python3 tools/test_upload_stability.py
python3 tools/test_log_concurrency.py  # needs complete dependencies already fetched
python3 tools/test_espuino_helper.py
UV_CONSTRAINT="$PWD/tools/build-constraints.txt" pio run -e complete -j 4
```

The simulations compile production bodies/classes, replacing hardware and transport APIs.
They are not a full ESP32 emulator. Sanitizers cover host memory and undefined arithmetic;
passing them does not establish absence of all FreeRTOS races or hardware faults.

## Device evidence and remaining limits

The retrieved dump reports `insert_free_block` / TLSF free-list corruption in `loopTask`.
Its application hash prefix `5551af21f` does not match the firmware running when fetched.
It therefore cannot prove which code location caused the reported volume crash. The dump
has been preserved; it was not erased or decoded against mismatching symbols as evidence.

The independent full-access API key is stored outside version control. `/info` and `/debug`
expose reset reason, heap watermarks, ELF identity and dump identity; release artifacts now
include exact ELF symbols for future crashes. Private raw logs/dumps remain outside this repo.

Physical maximum-volume playback, amplifier/power behavior, I2S timing, encoder bounce and
long-duration playback still require hardware observation. A paused volume sweep validates
control handling only. No claim is made that every possible crash has been eliminated.
