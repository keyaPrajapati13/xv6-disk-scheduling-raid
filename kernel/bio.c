// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents. Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//   so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "fs.h"
#include "buf.h"

#define DISK_REQ_READ  0
#define DISK_REQ_WRITE 1

// latency = |current_head - requested_block| + DISK_LATENCY_C
#define DISK_LATENCY_C 5
#define MAX_DISK_REQS 64

struct disk_request {
  struct buf *b;
  int blockno;
  int type;
  struct proc *proc;
  int used;
  int completed;
  struct disk_request *next;
};

static struct disk_request req_pool[MAX_DISK_REQS];

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf head;
} bcache;

struct {
  struct spinlock lock;
  struct disk_request *head;
  int busy;
} disk_queue;

int disk_sched_policy = 0; // 0 = FCFS, 1 = SSTF
int current_disk_head = 0;

uint64 disk_reads = 0;
uint64 disk_writes = 0;
uint64 disk_total_latency = 0;
uint64 disk_req_count = 0;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;

  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }

  initlock(&disk_queue.lock, "disk_queue");
  disk_queue.head = 0;
  disk_queue.busy = 0;

  for(int i = 0; i < MAX_DISK_REQS; i++){
    req_pool[i].used = 0;
    req_pool[i].completed = 0;
    req_pool[i].next = 0;
  }
}

// Must be called with disk_queue.lock held.
static struct disk_request *
alloc_req(void)
{
  for(int i = 0; i < MAX_DISK_REQS; i++){
    if(req_pool[i].used == 0){
      req_pool[i].used = 1;
      req_pool[i].completed = 0;
      req_pool[i].next = 0;
      return &req_pool[i];
    }
  }
  return 0;
}

// Must be called with disk_queue.lock held.
static void
free_req(struct disk_request *r)
{
  r->used = 0;
  r->completed = 0;
  r->next = 0;
}

// Must be called with disk_queue.lock held.
static void
enqueue_disk_request_locked(struct disk_request *req)
{
  req->next = 0;

  if(disk_queue.head == 0){
    disk_queue.head = req;
    return;
  }

  struct disk_request *cur = disk_queue.head;
  while(cur->next)
    cur = cur->next;
  cur->next = req;
}

// Must be called with disk_queue.lock held.
static struct disk_request *
dequeue_disk_request_locked(void)
{
  if(disk_queue.head == 0)
    return 0;

  struct disk_request *selected = disk_queue.head;
  struct disk_request *selected_prev = 0;

  if(disk_sched_policy == 1){
    struct disk_request *cur = disk_queue.head;
    struct disk_request *prev = 0;
    int min_dist = 0x7fffffff;

    while(cur){
      int dist = cur->blockno - current_disk_head;
      if(dist < 0)
        dist = -dist;

      int better = 0;
      if(dist < min_dist){
        better = 1;
      } else if(dist == min_dist){
        int cur_level = cur->proc ? cur->proc->level : 3;
        int sel_level = selected->proc ? selected->proc->level : 3;
        if(cur_level < sel_level)
          better = 1;
      }

      if(better){
        min_dist = dist;
        selected = cur;
        selected_prev = prev;
      }

      prev = cur;
      cur = cur->next;
    }
  }

  if(selected_prev == 0)
    disk_queue.head = selected->next;
  else
    selected_prev->next = selected->next;

  selected->next = 0;
  return selected;
}

static void
run_disk_dispatcher(void)
{
  for(;;){
    acquire(&disk_queue.lock);
    struct disk_request *r = dequeue_disk_request_locked();
    if(r == 0){
      disk_queue.busy = 0;
      wakeup(&disk_queue);
      release(&disk_queue.lock);
      return;
    }
    release(&disk_queue.lock);

    int dist = r->blockno - current_disk_head;
    if(dist < 0)
      dist = -dist;
    int latency = dist + DISK_LATENCY_C;

    acquire(&disk_queue.lock);
    current_disk_head = r->blockno;
    disk_total_latency += latency;
    disk_req_count++;
    if(r->type == DISK_REQ_READ)
      disk_reads++;
    else
      disk_writes++;
    release(&disk_queue.lock);

    virtio_disk_rw(r->b, (r->type == DISK_REQ_WRITE) ? 1 : 0);

    acquire(&disk_queue.lock);
    r->completed = 1;
    wakeup(r);
    release(&disk_queue.lock);
  }
}

void
submit_disk_request(struct buf *b, int type)
{
  acquire(&disk_queue.lock);

  struct disk_request *req = alloc_req();
  if(req == 0){
    release(&disk_queue.lock);
    panic("submit_disk_request: request pool exhausted");
  }

  req->b = b;
  req->blockno = b->blockno;
  req->type = type;
  req->proc = myproc();
  enqueue_disk_request_locked(req);

  if(disk_queue.busy == 0){
    disk_queue.busy = 1;
    release(&disk_queue.lock);
    run_disk_dispatcher();
    acquire(&disk_queue.lock);
  }

  while(req->completed == 0)
    sleep(req, &disk_queue.lock);

  free_req(req);
  release(&disk_queue.lock);
}

static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0){
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  panic("bget: no buffers");
}

struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid){
    submit_disk_request(b, DISK_REQ_READ);
    b->valid = 1;
  }
  return b;
}

void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");

  submit_disk_request(b, DISK_REQ_WRITE);
}

void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if(b->refcnt == 0){
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  release(&bcache.lock);
}

void
bpin(struct buf *b)
{
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b)
{
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}

void
set_disk_policy(int policy)
{
  acquire(&disk_queue.lock);
  disk_sched_policy = policy;
  release(&disk_queue.lock);
}

uint64
get_avg_disk_latency(void)
{
  acquire(&disk_queue.lock);
  uint64 avg = (disk_req_count > 0) ? (disk_total_latency / disk_req_count) : 0;
  release(&disk_queue.lock);
  return avg;
}
