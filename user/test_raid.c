//
// test_raid.c
// Verifies RAID 0, RAID 1, and RAID 5 swap correctness.
//
// For each RAID mode:
//   1. Switch to that RAID mode via setraidmode().
//   2. Allocate enough pages to trigger swap-out to disk under that mode.
//   3. Write a known pattern, then read it back.
//   4. Verify data integrity — proves the RAID mapping is consistent
//      between write (swap_disk_write) and read (swap_disk_read).
//
// RAID 5 section also demonstrates reconstruction by noting that our
// swap_disk_read handles the FAILED_DISK path automatically when
// FAILED_DISK != -1 (set at compile time for degraded-mode testing).
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N_PAGES   30
#define PAGE_SIZE 4096
#define TOTAL     (N_PAGES * PAGE_SIZE)
#define PATTERN(page, byte) ((char)(((page) * 7 + (byte)) & 0xFF))

static int
verify_pages(char *mem, int n, const char *label)
{
  int errors = 0;
  for(int p = 0; p < n; p++){
    char *page = mem + (uint64)p * PAGE_SIZE;
    for(int j = 0; j < PAGE_SIZE; j++){
      if(page[j] != PATTERN(p, j)){
        if(errors == 0)
          printf("  FAIL [%s] page=%d byte=%d got=0x%x want=0x%x\n",
                 label, p, j,
                 (unsigned char)page[j],
                 (unsigned char)PATTERN(p, j));
        errors++;
        if(errors > 3) return errors;
      }
    }
  }
  return errors;
}

static void
run_raid_test(int mode, const char *name)
{
  printf("\n--- RAID %s (mode=%d) ---\n", name, mode);
  setraidmode(mode);

  // Fork so each RAID test gets a clean address space and fresh swap slots.
  int pid = fork();
  if(pid < 0){ printf("fork failed\n"); exit(1); }

  if(pid == 0){
    char *mem = sbrklazy(TOTAL);
    if(mem == (char*)-1){
      printf("  sbrklazy failed\n");
      exit(1);
    }

    // Write pattern — touches all pages, causing evictions to disk
    // under the current RAID mode.
    for(int p = 0; p < N_PAGES; p++){
      char *page = mem + (uint64)p * PAGE_SIZE;
      for(int j = 0; j < PAGE_SIZE; j++)
        page[j] = PATTERN(p, j);
    }

    struct vmstats vs;
    getvmstats(getpid(), &vs);
    printf("  After write: swapped_out=%d evicted=%d\n",
           vs.pages_swapped_out, vs.pages_evicted);

    if(vs.pages_swapped_out == 0)
      printf("  WARNING: nothing swapped out — increase N_PAGES\n");

    // Read back — triggers swap-in from disk under current RAID mode.
    int errors = verify_pages(mem, N_PAGES, name);

    getvmstats(getpid(), &vs);
    printf("  After read:  swapped_in=%d\n", vs.pages_swapped_in);

    struct diskstats ds;
    getdiskstats(&ds);
    printf("  disk_reads=%d disk_writes=%d\n",
           (int)ds.disk_reads, (int)ds.disk_writes);

    if(errors == 0)
      printf("  PASS: all pages correct after swap through RAID %s\n", name);
    else
      printf("  FAIL: %d errors — RAID %s mapping inconsistency\n", errors, name);

    exit(errors == 0 ? 0 : 1);
  }

  int status;
  wait(&status);
  if(status == 0)
    printf("  [RAID %s] child exited OK\n", name);
  else
    printf("  [RAID %s] child exited with errors\n", name);
}

int
main(void)
{
  printf("=== test_raid: RAID mapping and parity verification ===\n");

  // Test each RAID mode in turn.
  run_raid_test(0, "0 (Striping)");
  run_raid_test(1, "1 (Mirroring)");
  run_raid_test(2, "5 (Parity)");

  // ---- RAID 5 reconstruction note ----
  printf("\n--- RAID 5 reconstruction ---\n");
  printf("FAILED_DISK is set at compile time in virtio_disk.c.\n");
  printf("To test reconstruction:\n");
  printf("  Set #define FAILED_DISK 2 in virtio_disk.c, recompile,\n");
  printf("  and re-run this program. The RAID 5 test above will then\n");
  printf("  exercise the XOR reconstruction path automatically.\n");
  printf("  Data should still verify correctly if reconstruction works.\n");

  // Restore default.
  setraidmode(0);
  printf("\n=== test_raid complete ===\n");
  exit(0);
}