#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "vm.h"
#include "proc.h"
extern struct proc proc[NPROC];
extern struct spinlock wait_lock;

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_hello(void)
{
	printf("Hello from the Kernel!\n");
	return 0;
}

uint64
sys_getpid2(void)
{
	return myproc()->pid;
}

uint64
sys_getppid(void)
{
	struct proc *p=myproc();
	if(p->parent)
		return p->parent->pid;
	return -1;
}

uint64
sys_getnumchild(void)
{
struct proc *p=myproc();
return getnumchild(p);


}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);  

  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_getsyscount(void){
return myproc()->syscount;
}


uint64
sys_getchildsyscount(void)
{
  int pid;
  struct proc *p;
  struct proc *curproc = myproc();

  argint(0, &pid);

  acquire(&wait_lock);
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->pid == pid &&
       p->parent == curproc &&
       p->state != UNUSED){
      uint64 cnt = p->syscount;
      release(&wait_lock);
      return cnt;
    }
  }
  release(&wait_lock);
  return (uint64)-1;
}

uint64
sys_getlevel(void)
{
    return myproc()->level;
}

uint64
sys_getmlfqinfo(void)
{
    int pid;
    uint64 addr;

    argint(0, &pid);
    argaddr(1, &addr);

    struct proc *p;

    for(p = proc; p < &proc[NPROC]; p++){
        if(p->pid == pid){
            struct mlfqinfo info;
            acquire(&p->lock);

            info.level = p->level;
            info.times_scheduled = p->times_scheduled;
            info.total_syscalls = p->syscount;

            for(int i = 0; i < 4; i++)
                info.ticks[i] = p->ticks[i];
            release(&p->lock);

            if(copyout(myproc()->pagetable, addr,
                       (char*)&info, sizeof(info)) < 0)
                return -1;

            return 0;
        }
    }

    return -1;
}

// Disk statistics — maintained in bio.c
extern uint64 disk_reads;
extern uint64 disk_writes;
extern uint64 get_avg_disk_latency(void);

uint64
sys_getdiskstats(void)
{
  uint64 addr;
  argaddr(0, &addr);

  struct diskstats ds;
  ds.disk_reads       = disk_reads;
  ds.disk_writes      = disk_writes;
  ds.avg_disk_latency = get_avg_disk_latency();

  if(copyout(myproc()->pagetable, addr, (char*)&ds, sizeof(ds)) < 0)
    return -1;

  return 0;
}

extern void set_disk_policy(int policy);

uint64
sys_setdisksched(void)
{
  int policy;
  argint(0, &policy);

  if(policy != 0 && policy != 1)
    return -1;  // only 0=FCFS and 1=SSTF are valid

  set_disk_policy(policy);
  return 0;
}

extern void set_raid_mode(int mode);

uint64
sys_setraidmode(void)
{
  int mode;
  argint(0, &mode);

  if(mode < 0 || mode > 2)
    return -1;  // 0=RAID0, 1=RAID1, 2=RAID5

  set_raid_mode(mode);
  return 0;
}

uint64
sys_getvmstats(void)
{
  int pid;
  uint64 addr;

  argint(0, &pid);   
  argaddr(1, &addr); 

  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    if(p->pid == pid){
      struct vmstats stats;
      acquire(&p->lock);

      stats.page_faults = p->page_faults;
      stats.pages_swapped_in = p->pages_swapped_in;
      stats.pages_swapped_out = p->pages_swapped_out;
      stats.pages_evicted = p->pages_evicted;
      stats.resident_pages = p->resident_pages;  // FIX Bug 1: was never assigned
      release(&p->lock);

      if(copyout(myproc()->pagetable, addr, (char*)&stats, sizeof(stats)) < 0)
        return -1;

      return 0;
    }
  }

  return -1;
}
