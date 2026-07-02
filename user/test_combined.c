//
// test_combined.c
// Combined workload test — the "example workload" from the spec:
//   Allocate large memory region
//   Trigger page faults
//   Observe disk activity and scheduling behaviour
//
// Also demonstrates scheduler-aware disk scheduling by running
// two processes at different MLFQ levels side by side.
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N_PAGES   30
#define PAGE_SIZE 4096
#define TOTAL     (N_PAGES * PAGE_SIZE)

static void
print_disk(const char *label)
{
  struct diskstats ds;
  getdiskstats(&ds);
  printf("[%s] disk_reads=%d disk_writes=%d avg_latency=%d\n",
         label,
         (int)ds.disk_reads,
         (int)ds.disk_writes,
         (int)ds.avg_disk_latency);
}

static void
print_vm(const char *label)
{
  struct vmstats vs;
  getvmstats(getpid(), &vs);
  printf("[%s] pid=%d faults=%d evicted=%d swapped_out=%d swapped_in=%d resident=%d\n",
         label, getpid(),
         vs.page_faults, vs.pages_evicted,
         vs.pages_swapped_out, vs.pages_swapped_in,
         vs.resident_pages);
}

// Worker: allocates TOTAL bytes, writes then reads every page.
// The write pass triggers page faults + evictions (swap-out).
// The read pass triggers swap-in for evicted pages.
static void
worker(int id, int policy)
{
  setdisksched(policy);
  printf("\n[worker %d] policy=%s  pid=%d\n",
         id, policy == 0 ? "FCFS" : "SSTF", getpid());

  print_disk("before alloc");
  print_vm("before alloc");

  // Step 1: allocate large region (lazy — no pages yet).
  char *mem = sbrklazy(TOTAL);
  if(mem == (char*)-1){
    printf("[worker %d] sbrklazy failed\n", id);
    exit(1);
  }
  printf("[worker %d] allocated %d pages lazily\n", id, N_PAGES);

  // Step 2: write full page content so evicted pages have unambiguous data.
  // Writing only first/last byte caused corruption: when a page was evicted
  // mid-write, the middle bytes were uninitialised garbage that got swapped
  // to disk, then read back and compared against the unwritten middle.
  printf("[worker %d] writing pages (triggers faults + swap-out)...\n", id);
  for(int p = 0; p < N_PAGES; p++){
    char *page = mem + (uint64)p * PAGE_SIZE;
    char val_first = (char)(id * 10 + p);
    char val_last  = (char)(~val_first);
    for(int j = 0; j < PAGE_SIZE; j++)
      page[j] = (j == PAGE_SIZE - 1) ? val_last : val_first;
  }

  print_vm("after write");
  print_disk("after write");

  // Step 3: read all pages back — triggers swap-in for evicted ones.
  printf("[worker %d] reading pages back (triggers swap-in)...\n", id);
  int errors = 0;
  for(int p = 0; p < N_PAGES; p++){
    char *page = mem + (uint64)p * PAGE_SIZE;
    char expected_first = (char)(id * 10 + p);
    char expected_last  = (char)(~expected_first);
    // Check first byte, last byte, and one middle byte.
    if(page[0] != expected_first ||
       page[PAGE_SIZE-1] != expected_last ||
       page[PAGE_SIZE/2] != expected_first){
      printf("[worker %d] FAIL page %d: first=0x%x(want 0x%x) mid=0x%x last=0x%x(want 0x%x)\n",
             id, p,
             (unsigned char)page[0],          (unsigned char)expected_first,
             (unsigned char)page[PAGE_SIZE/2],
             (unsigned char)page[PAGE_SIZE-1], (unsigned char)expected_last);
      errors++;
    }
  }

  print_vm("after read");
  print_disk("after read");

  if(errors == 0)
    printf("[worker %d] PASS: all pages verified\n", id);
  else
    printf("[worker %d] FAIL: %d page errors\n", id, errors);

  exit(errors);
}

int
main(void)
{
  printf("=== test_combined: large workload + disk scheduling observation ===\n");

  // ---- Part 1: single process, FCFS ----
  printf("\n== Part 1: single process under FCFS ==\n");
  int pid1 = fork();
  if(pid1 == 0) worker(1, 0);
  int st1; wait(&st1);

  // ---- Part 2: single process, SSTF ----
  printf("\n== Part 2: single process under SSTF ==\n");
  int pid2 = fork();
  if(pid2 == 0) worker(2, 1);
  int st2; wait(&st2);

  // ---- Part 3: two concurrent processes (scheduler-aware) ----
  // One process is naturally at a lower MLFQ level after doing
  // many syscalls; the kernel should prefer its disk requests.
  printf("\n== Part 3: two concurrent processes (scheduler-aware) ==\n");
  setdisksched(1); // SSTF with priority tie-break

  int pA = fork();
  if(pA == 0){
    // Child A: runs normally, will be at default level.
    printf("[child A] pid=%d level=%d\n", getpid(), getlevel());
    char *m = sbrklazy(TOTAL);
    if(m == (char*)-1) exit(1);
    for(int p = 0; p < N_PAGES; p++)
      m[p * PAGE_SIZE] = (char)p;
    // read back
    for(int p = 0; p < N_PAGES; p++){
      volatile char x = m[p * PAGE_SIZE];
      (void)x;
    }
    printf("[child A] done, level=%d\n", getlevel());
    exit(0);
  }

  int pB = fork();
  if(pB == 0){
    // Child B: same workload, different pid.
    printf("[child B] pid=%d level=%d\n", getpid(), getlevel());
    char *m = sbrklazy(TOTAL);
    if(m == (char*)-1) exit(1);
    for(int p = 0; p < N_PAGES; p++)
      m[p * PAGE_SIZE] = (char)(p + 1);
    for(int p = 0; p < N_PAGES; p++){
      volatile char x = m[p * PAGE_SIZE];
      (void)x;
    }
    printf("[child B] done, level=%d\n", getlevel());
    exit(0);
  }

  int stA, stB;
  wait(&stA);
  wait(&stB);

  print_disk("final");
  printf("\n=== test_combined complete ===\n");

  // Restore defaults.
  setdisksched(0);
  setraidmode(0);
  exit(0);
}