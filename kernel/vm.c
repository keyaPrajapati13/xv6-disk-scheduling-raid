#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

extern struct frame frame_table[];

// NOTE: swap_space[] removed — PA4 uses disk-backed swap, not in-memory array.
extern int swap_used[MAX_SWAP_PAGES];

extern struct swap_entry swap_map[MAX_SWAP_PAGES];

extern struct spinlock frame_lock;

extern int user_frame_count;

extern int swap_disk_read(int swap_slot, char *dst);

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--)
  {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V)
    {
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;)
  {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
//
// FIX (bug #15): Also frees any swap slots for pages that were
// evicted to disk (PTE is 0 but swap_map entry exists). Without
// this, exiting/shrinking processes leak all their swap slots.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  struct proc *p = myproc();

  // BUG FIX: when freeproc(child) runs inside the parent's wait(), myproc()
  // returns the PARENT. The swap cleanup loop would then match
  // (proc == parent, va == a) and accidentally free the PARENT's own swap
  // entries for any VA it shares with the dead child.
  //
  // Guard: only clean up swap slots if this pagetable belongs to myproc().
  // For the freeproc path, free_proc_swap_slots() has already cleared the
  // child's entries before proc_freepagetable() is called, so suppressing
  // cleanup here is both correct and safe.
  if (p != 0 && p->pagetable != pagetable)
    p = 0; // suppress swap cleanup for a pagetable we don't own

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
  {
    // Free swap slots for evicted pages even when PTE is not present.
    // Swapped-out pages have PTE cleared to 0, so the normal path misses them.
    if (do_free && p != 0)
    {
      acquire(&frame_lock);
      for (int i = 0; i < MAX_SWAP_PAGES; i++)
      {
        if (swap_map[i].used &&
            swap_map[i].proc == p &&
            swap_map[i].va == a)
        {
          swap_used[swap_map[i].swap_slot] = 0;
          swap_map[i].used = 0;
          swap_map[i].proc = 0;
          swap_map[i].va = 0;
          swap_map[i].swap_slot = -1;
          break;
        }
      }
      release(&frame_lock);
    }

    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_V) == 0) // has physical page been allocated?
    {
      *pte = 0;
      continue;
    }
    if (do_free)
    {
      uint64 pa = PTE2PA(*pte);
      // Clean up frame table entry for this user page
      acquire(&frame_lock);
      for (int i = 0; i < MAX_FRAMES; i++)
      {
        if (frame_table[i].used && frame_table[i].user_frame &&
            frame_table[i].proc == p &&   // FIX: also match proc to avoid
            frame_table[i].va == a)       // clearing another proc's frame
        {
          frame_table[i].used = 0;
          frame_table[i].user_frame = 0;
          frame_table[i].proc = 0;
          frame_table[i].va = 0;
          frame_table[i].ref_bit = 0;
          if (user_frame_count > 0)
            user_frame_count--;
          break;
        }
      }
      release(&frame_lock);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0)
    {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy its memory into a child's
// page table. Copies both the page table and the physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
//
// FIX (bug #16): Also copies swap_map entries for pages that were
// evicted to disk. Without this, forked children lose access to any
// parent pages that were swapped out at fork time.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz, struct proc *child)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  // parent is the caller (fork context); child is the new proc being set up.
  struct proc *parent = myproc();

  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walk(old, i, 0)) == 0 || (*pte & PTE_V) == 0)
    {
      // No resident mapping — check if it was swapped out.
      // If so, allocate a new swap slot for the child and copy the data.
      if (parent != 0)
      {
        acquire(&frame_lock);
        int src_slot = -1;
        for (int s = 0; s < MAX_SWAP_PAGES; s++)
        {
          if (swap_map[s].used &&
              swap_map[s].proc == parent &&
              swap_map[s].va == i)
          {
            src_slot = swap_map[s].swap_slot;
            break;
          }
        }
        release(&frame_lock);

        if (src_slot != -1)
        {
          // Read parent's swapped page into a temporary buffer.
          char *tmp = kalloc();
          if (tmp == 0)
            goto err;

          if (swap_disk_read(src_slot, tmp) < 0)
          {
            kfree(tmp);
            goto err;
          }

          // Find a free swap slot for the child.
          acquire(&frame_lock);
          int dst_slot = -1;
          for (int s = 0; s < MAX_SWAP_PAGES; s++)
          {
            if (swap_used[s] == 0)
            {
              dst_slot = s;
              swap_used[s] = 1;
              break;
            }
          }
          release(&frame_lock);

          if (dst_slot == -1)
          {
            kfree(tmp);
            goto err;
          }

          // Write to child's swap slot via disk.
          extern int swap_disk_write(int swap_slot, char *src);
          if (swap_disk_write(dst_slot, tmp) < 0)
          {
            acquire(&frame_lock);
            swap_used[dst_slot] = 0;
            release(&frame_lock);
            kfree(tmp);
            goto err;
          }

          kfree(tmp);

          // Register swap_map entry for the CHILD directly.
          // BUG WAS HERE: original code set proc=parent with a "fix up in fork"
          // comment, but kfork() never had any fixup code. The child's swap
          // entries permanently pointed to the parent, so vmfault could never
          // find them when the child page-faulted → spurious child crash.
          acquire(&frame_lock);
          int inserted = 0;
          for (int s = 0; s < MAX_SWAP_PAGES; s++)
          {
            if (swap_map[s].used == 0)
            {
              swap_map[s].used      = 1;
              swap_map[s].proc      = child; // FIX: use child, not parent
              swap_map[s].va        = i;
              swap_map[s].swap_slot = dst_slot;
              inserted = 1;
              break;
            }
          }
          if(inserted == 0)
            swap_used[dst_slot] = 0;
          release(&frame_lock);

          if(inserted == 0)
            goto err;
        }
      }
      continue; // physical page hasn't been allocated
    }

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
    {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
    {
      if ((pa0 = vmfault(pagetable, va0, 0)) == 0)
      {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    if (pte == 0)
      return -1;
    // forbid copyout over read-only user text pages.
    if ((*pte & PTE_W) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
    {
      if ((pa0 = vmfault(pagetable, va0, 0)) == 0)
      {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0)
    {
      if (*p == '\0')
      {
        *dst = '\0';
        got_null = 1;
        break;
      }
      else
      {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null)
  {
    return 0;
  }
  else
  {
    return -1;
  }
}

int ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if (*pte & PTE_V)
    return 1;
  return 0;
}

// Handle a page fault or demand-fault for virtual address va.
//
// Returns the physical address of the mapped page on success, 0 on failure.
// Callers (usertrap, copyout, copyin) use 0 as the error sentinel.
//
// FIX (bug #14): The old code returned literal 1 for already-mapped pages.
// copyout/copyin use the return value as a physical address, so returning 1
// caused memmove to write to address 0x1 — an instant kernel crash.
// Now we return walkaddr() which gives the real physical address.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (p == 0)
    return 0;

  if (va >= p->sz)
    return 0;

  va = PGROUNDDOWN(va);

  // Case 1: already mapped — just refresh the ref_bit and return the real PA.
  // BUG WAS HERE: old code returned literal 1 instead of the physical address.
  if (ismapped(pagetable, va))
  {
    acquire(&frame_lock);
    for (int i = 0; i < MAX_FRAMES; i++)
    {
      if (frame_table[i].used &&
          frame_table[i].proc == p &&
          frame_table[i].va == va)
      {
        frame_table[i].ref_bit = 1;
        break;
      }
    }
    release(&frame_lock);
    // Return the actual physical address, not a placeholder 1.
    return walkaddr(pagetable, va);
  }

  p->page_faults++;

  // STEP 1: Check if page was swapped to disk.
  int found_swap = -1;
  int swap_slot = -1;

  acquire(&frame_lock);
  for (int i = 0; i < MAX_SWAP_PAGES; i++)
  {
    if (swap_map[i].used &&
        swap_map[i].proc == p &&
        swap_map[i].va == va)
    {
      found_swap = i;
      swap_slot = swap_map[i].swap_slot;
      break;
    }
  }
  release(&frame_lock);

  if (found_swap != -1)
  {
    // Allocate a physical frame for this process/va.
    mem = (uint64)kalloc_user(p, va);
    if (mem == 0)
      return 0;

    // Read the page back from disk via RAID-mapped swap.
    if (swap_disk_read(swap_slot, (char *)mem) < 0)
    {
      free_user_frame(p, va, (void *)mem);
      return 0;
    }

    // Map the newly-read page into the process page table.
    if (mappages(p->pagetable, va, PGSIZE, mem,
                 PTE_W | PTE_U | PTE_R) != 0)
    {
      free_user_frame(p, va, (void *)mem);
      return 0;
    }

    // Mark ref_bit = 1 and release the swap slot.
    acquire(&frame_lock);
    for (int i = 0; i < MAX_FRAMES; i++)
    {
      if (frame_table[i].used &&
          frame_table[i].proc == p &&
          frame_table[i].va == va)
      {
        frame_table[i].ref_bit = 1;
        break;
      }
    }

    // Release the swap slot now that the page is back in RAM.
    swap_used[swap_slot] = 0;
    swap_map[found_swap].used = 0;
    swap_map[found_swap].proc = 0;
    swap_map[found_swap].va = 0;
    swap_map[found_swap].swap_slot = -1;

    release(&frame_lock);

    p->pages_swapped_in++;
    p->resident_pages++;

    return mem;
  }

  // STEP 2: Lazy allocation — page was never allocated yet.
  mem = (uint64)kalloc_user(p, va);
  if (mem == 0)
    return 0;

  memset((void *)mem, 0, PGSIZE);

  if (mappages(p->pagetable, va, PGSIZE, mem,
               PTE_W | PTE_U | PTE_R) != 0)
  {
    free_user_frame(p, va, (void *)mem);
    return 0;
  }

  // Set ref_bit for the new frame.
  acquire(&frame_lock);
  for (int i = 0; i < MAX_FRAMES; i++)
  {
    if (frame_table[i].used &&
        frame_table[i].proc == p &&
        frame_table[i].va == va)
    {
      frame_table[i].ref_bit = 1;
      break;
    }
  }
  release(&frame_lock);

  p->resident_pages++;

  return mem;
}
