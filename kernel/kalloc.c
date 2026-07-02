// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "vm.h"

extern int swap_disk_write(int swap_slot, char *src);

// Frame table: one entry per user frame slot.
struct frame frame_table[MAX_FRAMES];

// Clock hand for Clock page replacement.
int clock_hand = 0;

// swap_used[i] == 1 means disk swap slot i is occupied.
int swap_used[MAX_SWAP_PAGES];

// swap_map[i]: records which (proc, va) is stored in swap slot i.
struct swap_entry swap_map[MAX_SWAP_PAGES];

// Number of user frames currently allocated.
int user_frame_count = 0;

// Lock protecting frame_table, swap_map, swap_used, user_frame_count.
struct spinlock frame_lock;

static int
register_user_frame(struct proc *p, uint64 va)
{
  for(int i = 0; i < MAX_FRAMES; i++){
    if(frame_table[i].used == 0){
      frame_table[i].used = 1;
      frame_table[i].user_frame = 1;
      frame_table[i].proc = p;
      frame_table[i].va = va;
      frame_table[i].ref_bit = 1;
      user_frame_count++;
      return 0;
    }
  }
  return -1;
}

static int
has_free_frame_slot(void)
{
  for(int i = 0; i < MAX_FRAMES; i++){
    if(frame_table[i].used == 0)
      return 1;
  }
  return 0;
}

static void
restore_victim_frame(int victim, struct proc *evict_proc, uint64 evict_va)
{
  frame_table[victim].used = 1;
  frame_table[victim].user_frame = 1;
  frame_table[victim].proc = evict_proc;
  frame_table[victim].va = evict_va;
  frame_table[victim].ref_bit = 1;
}

// Select a victim user frame using the Clock algorithm.
// Prefers to evict frames belonging to lower-priority processes
// (higher MLFQ level number = lower priority).
// Must be called with frame_lock held.
static int
select_victim_frame(void)
{
  int best = -1;
  int best_level = -1;

  for(int scanned = 0; scanned < MAX_FRAMES * 2; scanned++){
    struct frame *f = &frame_table[clock_hand];
    int idx = clock_hand;
    clock_hand = (clock_hand + 1) % MAX_FRAMES;

    if(!f->used || !f->user_frame)
      continue;

    if(f->ref_bit == 1){
      f->ref_bit = 0;
      continue;
    }

    struct proc *owner = f->proc;
    int plevel = (owner != 0) ? owner->level : 0;

    if(plevel > best_level){
      best = idx;
      best_level = plevel;
    }

    if(best_level == 3)
      break;
  }

  if(best == -1){
    for(int i = 0; i < MAX_FRAMES; i++){
      if(frame_table[i].used && frame_table[i].user_frame){
        best = i;
        break;
      }
    }
  }

  return best;
}

// Evict one tracked user frame to disk-backed swap.
// Returns 0 on success, -1 on failure.
static int
evict_one_user_frame(void)
{
  for(int attempts = 0; attempts < MAX_FRAMES; attempts++){
    acquire(&frame_lock);

    int victim = select_victim_frame();
    if(victim < 0){
      release(&frame_lock);
      return -1;
    }

    struct frame *f = &frame_table[victim];
    struct proc *evict_proc = f->proc;
    uint64 evict_va = f->va;
    int reserved_swap_slot = -1;

    f->user_frame = 0;
    f->proc = 0;
    f->va = 0;
    f->ref_bit = 0;

    for(int i = 0; i < MAX_SWAP_PAGES; i++){
      if(swap_used[i] == 0){
        reserved_swap_slot = i;
        swap_used[i] = 1;
        break;
      }
    }

    if(reserved_swap_slot == -1){
      restore_victim_frame(victim, evict_proc, evict_va);
      release(&frame_lock);
      return -1;
    }

    release(&frame_lock);

    if(evict_proc == 0 || evict_proc->pagetable == 0){
      acquire(&frame_lock);
      swap_used[reserved_swap_slot] = 0;
      frame_table[victim].used = 0;
      frame_table[victim].user_frame = 0;
      frame_table[victim].proc = 0;
      frame_table[victim].va = 0;
      frame_table[victim].ref_bit = 0;
      if(user_frame_count > 0)
        user_frame_count--;
      release(&frame_lock);
      continue;
    }

    pte_t *pte = walk(evict_proc->pagetable, evict_va, 0);
    if(pte == 0 || (*pte & PTE_V) == 0){
      acquire(&frame_lock);
      swap_used[reserved_swap_slot] = 0;
      frame_table[victim].used = 0;
      frame_table[victim].user_frame = 0;
      frame_table[victim].proc = 0;
      frame_table[victim].va = 0;
      frame_table[victim].ref_bit = 0;
      if(user_frame_count > 0)
        user_frame_count--;
      release(&frame_lock);
      continue;
    }

    uint64 evict_pa = PTE2PA(*pte);
    pte_t oldpte = *pte;
    *pte = 0;
    sfence_vma();

    if(swap_disk_write(reserved_swap_slot, (char *)evict_pa) < 0){
      *pte = oldpte;
      sfence_vma();
      acquire(&frame_lock);
      swap_used[reserved_swap_slot] = 0;
      restore_victim_frame(victim, evict_proc, evict_va);
      release(&frame_lock);
      return -1;
    }

    acquire(&frame_lock);
    swap_map[reserved_swap_slot].used = 1;
    swap_map[reserved_swap_slot].proc = evict_proc;
    swap_map[reserved_swap_slot].va = evict_va;
    swap_map[reserved_swap_slot].swap_slot = reserved_swap_slot;
    frame_table[victim].used = 0;
    if(user_frame_count > 0)
      user_frame_count--;
    release(&frame_lock);

    evict_proc->pages_evicted++;
    if(evict_proc->resident_pages > 0)
      evict_proc->resident_pages--;
    evict_proc->pages_swapped_out++;

    kfree((void *)evict_pa);
    return 0;
  }

  return -1;
}

void freerange(void *pa_start, void *pa_end);
extern char end[];

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit(void)
{
  initlock(&kmem.lock, "kmem");
  initlock(&frame_lock, "frame_lock");
  freerange(end, (void *)PHYSTOP);

  user_frame_count = 0;
  clock_hand = 0;

  for(int i = 0; i < MAX_FRAMES; i++){
    frame_table[i].used = 0;
    frame_table[i].user_frame = 0;
    frame_table[i].proc = 0;
    frame_table[i].va = 0;
    frame_table[i].ref_bit = 0;
  }
  for(int i = 0; i < MAX_SWAP_PAGES; i++){
    swap_used[i] = 0;
    swap_map[i].used = 0;
    swap_map[i].proc = 0;
    swap_map[i].va = 0;
    swap_map[i].swap_slot = -1;
  }
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p = (char *)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

void
kfree(void *pa)
{
  struct run *r;
  if(((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

void
free_user_frame(struct proc *p, uint64 va, void *pa)
{
  acquire(&frame_lock);
  for(int i = 0; i < MAX_FRAMES; i++){
    if(frame_table[i].used &&
       frame_table[i].user_frame &&
       frame_table[i].proc == p &&
       frame_table[i].va == va){
      frame_table[i].used = 0;
      frame_table[i].user_frame = 0;
      frame_table[i].proc = 0;
      frame_table[i].va = 0;
      frame_table[i].ref_bit = 0;
      if(user_frame_count > 0)
        user_frame_count--;
      break;
    }
  }
  release(&frame_lock);

  kfree(pa);
}

// Standard kalloc for kernel allocations only.
void *
kalloc(void)
{
  struct run *r;
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char *)r, 5, PGSIZE);

  return (void *)r;
}

// kalloc_user allocates a physical frame for a user page.
// When memory is full, it evicts one resident user page to disk-backed swap.
void *
kalloc_user(struct proc *p, uint64 va)
{
  struct run *r = 0;

  for(int attempts = 0; attempts < 2; attempts++){
    acquire(&frame_lock);
    int need_evict = (user_frame_count >= MAX_FRAMES) || !has_free_frame_slot();
    release(&frame_lock);

    if(need_evict && evict_one_user_frame() < 0)
      return 0;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if(r)
      kmem.freelist = r->next;
    release(&kmem.lock);

    if(r == 0){
      acquire(&frame_lock);
      int can_retry = (user_frame_count > 0);
      release(&frame_lock);

      if(can_retry == 0 || evict_one_user_frame() < 0)
        return 0;
      continue;
    }

    memset((char *)r, 0, PGSIZE);

    acquire(&frame_lock);
    if(register_user_frame(p, va) == 0){
      release(&frame_lock);
      return (void *)r;
    }
    release(&frame_lock);

    kfree((void *)r);
    r = 0;

    if(evict_one_user_frame() < 0)
      return 0;
  }

  return 0;
}
