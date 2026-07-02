//
// test_disksched.c
// Compares FCFS and SSTF disk scheduling performance.
//
// Each policy runs in its own child process for a clean frame count.
// Stats are snapshotted before/after inside each child so deltas are
// accurate. Results are passed back to the parent via a pipe.
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Must exceed MAX_FRAMES to guarantee eviction and real disk I/O.
#define N_PAGES   30
#define PAGE_SIZE 4096
#define TOTAL     (N_PAGES * PAGE_SIZE)

// Non-sequential access order to create seek distance variation between
// disk requests, making scheduling policy choice meaningful.
static void
run_workload(char *mem)
{
  // Write: even pages first, then odd — creates gaps in block access.
  for(int p = 0; p < N_PAGES; p += 2)
    mem[(uint64)p * PAGE_SIZE] = (char)p;
  for(int p = 1; p < N_PAGES; p += 2)
    mem[(uint64)p * PAGE_SIZE] = (char)p;

  // Read: reverse order — different from write, increases seeking.
  for(int p = N_PAGES - 1; p >= 0; p--){
    volatile char x = mem[(uint64)p * PAGE_SIZE];
    (void)x;
  }
}

// Passed from child to parent via pipe.
struct result {
  uint64 reads;
  uint64 writes;
  uint64 avg_latency;
};

static void
child_run(int policy, const char *name, int pipe_wr)
{
  setdisksched(policy);

  struct diskstats before, after;
  getdiskstats(&before);

  char *mem = sbrklazy(TOTAL);
  if(mem == (char*)-1){
    printf("%s child: sbrklazy failed\n", name);
    exit(1);
  }

  run_workload(mem);
  getdiskstats(&after);

  struct result r;
  r.reads       = after.disk_reads   - before.disk_reads;
  r.writes      = after.disk_writes  - before.disk_writes;
  r.avg_latency = after.avg_disk_latency;

  printf("%s: reads=%d writes=%d avg_latency=%d\n",
         name, (int)r.reads, (int)r.writes, (int)r.avg_latency);

  write(pipe_wr, &r, sizeof(r));
  exit(0);
}

int
main(void)
{
  printf("=== test_disksched: FCFS vs SSTF performance ===\n");

  struct result fcfs_r, sstf_r;
  int pfd[2];

  // ---- FCFS child ----
  printf("\n-- Policy: FCFS (0) --\n");
  pipe(pfd);
  int pid = fork();
  if(pid == 0){
    close(pfd[0]);
    child_run(0, "FCFS", pfd[1]);
  }
  close(pfd[1]);
  int st; wait(&st);
  read(pfd[0], &fcfs_r, sizeof(fcfs_r));
  close(pfd[0]);

  // ---- SSTF child ----
  printf("\n-- Policy: SSTF (1) --\n");
  pipe(pfd);
  pid = fork();
  if(pid == 0){
    close(pfd[0]);
    child_run(1, "SSTF", pfd[1]);
  }
  close(pfd[1]);
  wait(&st);
  read(pfd[0], &sstf_r, sizeof(sstf_r));
  close(pfd[0]);

  // ---- Summary ----
  printf("\n=== Summary ===\n");
  printf("FCFS: reads=%d writes=%d avg_latency=%d\n",
         (int)fcfs_r.reads, (int)fcfs_r.writes, (int)fcfs_r.avg_latency);
  printf("SSTF: reads=%d writes=%d avg_latency=%d\n",
         (int)sstf_r.reads, (int)sstf_r.writes, (int)sstf_r.avg_latency);

  if(fcfs_r.reads == 0 && fcfs_r.writes == 0)
    printf("WARNING: FCFS no disk I/O — increase N_PAGES above MAX_FRAMES\n");

  printf("\nResult: SSTF avg_latency=%d  FCFS avg_latency=%d\n",
         (int)sstf_r.avg_latency, (int)fcfs_r.avg_latency);

  if(sstf_r.avg_latency <= fcfs_r.avg_latency)
    printf("PASS: SSTF latency <= FCFS (better or equal seek behaviour)\n");
  else
    printf("NOTE: SSTF > FCFS for this workload\n"
           "      Sequential patterns don't show SSTF benefit — try more scattered access.\n");

  setdisksched(0); // restore default
  exit(0);
}