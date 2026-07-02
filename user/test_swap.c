//
// test_swap.c
// Demonstrates and verifies disk-backed swap-out and swap-in.
//
// Strategy:
//   1. Allocate a memory region large enough to exceed MAX_FRAMES,
//      forcing the kernel to evict (swap out) some pages to disk.
//   2. Write a known pattern to every page before eviction.
//   3. Read every page back — pages that were evicted must be
//      swapped back in from disk.
//   4. Verify the data is intact (swap-in correctness).
//   5. Print vm stats before and after to show the counters moved.
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


#define N_PAGES   50
#define PAGE_SIZE 4096
#define TOTAL     (N_PAGES * PAGE_SIZE)

// Unique byte pattern for page p.
#define PATTERN(p) ((char)(0xA0 + (p)))

int
main(void)
{
  printf("=== test_swap: disk-backed swap correctness ===\n");

  int pid = getpid();

  // ---- baseline stats ----
  struct vmstats before;
  getvmstats(pid, &before);
  printf("\nBEFORE: faults=%d evicted=%d swapped_out=%d swapped_in=%d resident=%d\n",
         before.page_faults, before.pages_evicted,
         before.pages_swapped_out, before.pages_swapped_in,
         before.resident_pages);

  // ---- allocate and write ----
  char *mem = sbrklazy(TOTAL);
  if(mem == (char*)-1){
    printf("\nFAIL: sbrklazy failed\n");
    exit(1);
  }

  printf("\nWriting pattern to %d pages...\n", N_PAGES);
  for(int p = 0; p < N_PAGES; p++){
    char *page = mem + p * PAGE_SIZE;
    for(int j = 0; j < PAGE_SIZE; j++)
      page[j] = PATTERN(p);
  }

  struct vmstats mid;
  getvmstats(pid, &mid);
  printf("\nAFTER WRITE: faults=%d evicted=%d swapped_out=%d resident=%d\n",
         mid.page_faults, mid.pages_evicted,
         mid.pages_swapped_out, mid.resident_pages);

  if(mid.pages_swapped_out == 0)
    printf("\nWARNING: no pages swapped out — increase N_PAGES or decrease MAX_FRAMES\n");

  // ---- read back and verify ----
  printf("\nVerifying data integrity (triggers swap-in)...\n");
  int errors = 0;
  for(int p = 0; p < N_PAGES; p++){
    char *page = mem + p * PAGE_SIZE;
    for(int j = 0; j < PAGE_SIZE; j++){
      if(page[j] != PATTERN(p)){
        printf("\nFAIL: page %d byte %d: got 0x%x want 0x%x\n",
               p, j, (unsigned char)page[j], (unsigned char)PATTERN(p));
        errors++;
        if(errors > 5) goto done;
      }
    }
  }

done:
  // ---- final stats ----
  struct vmstats after;
  getvmstats(pid, &after);
  printf("\nAFTER READ: faults=%d evicted=%d swapped_out=%d swapped_in=%d resident=%d\n",
         after.page_faults, after.pages_evicted,
         after.pages_swapped_out, after.pages_swapped_in,
         after.resident_pages);

  if(errors == 0)
    printf("\nPASS: all %d pages verified correctly after swap-in\n", N_PAGES);
  else
    printf("\nFAIL: %d byte errors detected\n", errors);

  // ---- disk stats ----
  struct diskstats ds;
  getdiskstats(&ds);
  printf("\nDISK: reads=%d writes=%d avg_latency=%d\n",
         (int)ds.disk_reads, (int)ds.disk_writes, (int)ds.avg_disk_latency);

  exit(0);
}