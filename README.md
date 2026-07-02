# Disk Scheduling and RAID-Backed Swap in xv6

## Overview

This repository contains the PA4 implementation for CS3523: Operating Systems II at IIT Hyderabad, built on top of earlier xv6 assignments (PA1–PA3).

The completed system supports:
- Disk-backed swap instead of in-memory swap
- FCFS and SSTF disk scheduling with a priority-aware request queue
- Scheduler-aware disk scheduling tie-breaking
- RAID 0, RAID 1, and RAID 5 backed swap storage
- Integration with lazy allocation, page faults, and fork
- VM and disk statistics for experimental validation

---

## Implemented Features

### 1. Disk-Backed Swap

The swap path is fully disk-backed. Evicted user pages are written to swap slots on disk and tracked using `swap_used[]` and `swap_map[]`. The main swap logic is split across `kernel/kalloc.c` and `kernel/vm.c`.

Key points:
- Resident user pages are capped by `MAX_FRAMES = 20`
- `kalloc_user()` allocates tracked user frames and evicts when needed
- Eviction uses the frame table and writes victim pages through `swap_disk_write()`
- Page fault handling in `vmfault()` swaps pages back in through `swap_disk_read()`
- Failed swap or mapping paths roll back cleanly instead of leaking frame-table or swap state
- `free_user_frame()` cleans tracked frame state when a fault path fails after allocation

---

### 2. Disk Scheduling

Disk scheduling is implemented in `kernel/bio.c` using a real pending request queue. Requests are not executed immediately on enqueue, so scheduling policy has an actual effect.

Implemented policies:
- `FCFS` (`policy = 0`): First-come, first-served
- `SSTF` (`policy = 1`): Shortest seek time first

Additional behaviour:
- Seek latency model: `|current_head - requested_block| + C` where `C = 5`
- Average latency is tracked through cumulative disk statistics
- SSTF tie-breaking prefers the higher-priority process when distances are equal

---

### 3. RAID-Backed Swap

All swap I/O goes through the RAID layer in `kernel/virtio_disk.c`. Four logical disks are simulated in software on top of the xv6 disk image.

Implemented modes:
- **RAID 0**: Striping across 4 disks
- **RAID 1**: Mirrored pairs
- **RAID 5**: Rotating parity with read-modify-write parity updates

RAID 5 details:
- Rotating parity disk per stripe
- Correct data-disk mapping across non-parity disks
- Degraded read reconstruction by XOR of surviving blocks
- Degraded write handling for both missing-data and missing-parity cases

Current configuration:
- `FAILED_DISK` is set to `2`, applied only when `raid_mode == RAID5`
- RAID 5 reconstruction is exercised by the final validation run

---

### 4. Integration with the Virtual Memory System

- `vmfault()` handles both lazy allocation and swapped-out pages
- `uvmcopy()` correctly preserves swapped-out pages across `fork()`
- `sbrk()` uses lazy allocation to match the provided tests
- Resident-page accounting and swap counters exposed through `getvmstats()`
- Timer/scheduler behaviour in `trap.c` cleaned up to avoid double-yield issues

---

### 5. Syscalls and Statistics

Available interfaces:
- `getdiskstats(struct diskstats *info)`
- `setdisksched(int policy)`
- `setraidmode(int mode)`

Tracked memory statistics: `page_faults`, `pages_evicted`, `pages_swapped_out`, `pages_swapped_in`, `resident_pages`

Tracked disk statistics: `disk_reads`, `disk_writes`, `avg_disk_latency`

---

## Locking and Code Quality

- `frame_lock` protects `frame_table`, `swap_map`, `swap_used`, and `user_frame_count`
- `disk_queue.lock` protects the pending disk request queue, head position, and disk stats
- Failed fault/eviction paths explicitly roll back state instead of partially committing changes
- Disk latency accounting is statistical only, with no artificial busy-wait loop

---

## Files Modified

### Kernel
`kernel/bio.c`, `kernel/defs.h`, `kernel/kalloc.c`, `kernel/proc.c`, `kernel/sysproc.c`, `kernel/trap.c`, `kernel/virtio_disk.c`, `kernel/vm.c`

### User
`user/ulib.c`, `user/user.h`, `user/user.pl`

---

## Experimental Validation

### test_swap
```
BEFORE:     faults=2  evicted=0  swapped_out=0  swapped_in=0  resident=2
AFTER WRITE: faults=52 evicted=32 swapped_out=32 resident=20
AFTER READ:  faults=86 evicted=66 swapped_out=66 swapped_in=34 resident=20
DISK:        reads=309 writes=273 avg_latency=10
```
Confirms correct disk-backed eviction, swap-out, swap-in, and resident-page limiting.

### test_disksched
```
FCFS: reads=56 writes=80 avg_latency=13
SSTF: reads=32 writes=80 avg_latency=12
```
Confirms SSTF reduces average seek latency over FCFS for a scattered-access workload.

### test_raid
```
RAID 0: disk_reads=134 disk_writes=97
RAID 1: disk_reads=310 disk_writes=273
RAID 5: disk_reads=400 disk_writes=405
```
All three modes passed end-to-end data verification. RAID 5 run confirms degraded reconstruction with `FAILED_DISK = 2`.

### test_combined
- FCFS single-process phase: passed
- SSTF single-process phase: passed
- Concurrent two-process phase: completed successfully

---

## Notes
- RAID mode should be selected before a workload begins. Changing modes mid-run while pages are already swapped is unsafe.
- Disk statistics are system-wide and cumulative — use before/after snapshots for comparison.
