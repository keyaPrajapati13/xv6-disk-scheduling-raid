//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0
//          -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

// FIX (bug #5): disk_request is defined only in bio.c.
// Do NOT redefine struct disk_request here — it caused duplicate-symbol
// conflicts and potential ABI mismatches between the two files.

// FIX (bug #4): DISK_REQ_READ/WRITE defined only in bio.c.
// Use the same numeric values without redefining the macros.

#define NDISKS 4

// RAID modes
#define RAID0 0
#define RAID1 1
#define RAID5 2

// Simulate a single failed disk for RAID5 reconstruction testing.
// Keep this active only while raid_mode == RAID5 so RAID0/1 tests still
// exercise their normal paths.
#define FAILED_DISK 2

// FIX (bug #9): Single global RAID mode instead of per-slot mode cycling.
// The old get_raid_mode(swap_slot) = swap_slot % 3 silently changed RAID
// mode between slots, making the storage layout unpredictable and breaking
// read-back correctness (write in RAID0, read in RAID1, etc.).
int raid_mode = RAID0; // default; changed via set_raid_mode()

void set_raid_mode(int mode)
{
  raid_mode = mode;
}

#define SWAP_START_BLOCK 1000         // block offset past the filesystem
#define BLOCKS_PER_PAGE  (PGSIZE / BSIZE)

// FIX (kernel stack overflow): RAID 5 needs BSIZE-sized scratch buffers.
// Declaring them on the kernel stack inside a loop that runs BLOCKS_PER_PAGE
// times overflows xv6's 4 KB kernel stack, causing scause=0xf panics.
// Static globals live in BSS (not stack) — safe because swap_disk_read/write
// are called with the virtio sleeplock held, so they never run concurrently.
static char raid5_old_data[BSIZE];
static char raid5_old_parity[BSIZE];
static char raid5_new_parity[BSIZE];
static char raid5_recovered[BSIZE];

static int
current_failed_disk(void)
{
  if(raid_mode == RAID5)
    return FAILED_DISK;
  return -1;
}

static uint
swap_phys_block(int disk, int disk_block)
{
  // Simulate NDISKS by laying each stripe row out as NDISKS consecutive
  // blocks on the single xv6 disk image.
  return SWAP_START_BLOCK + disk_block * NDISKS + disk;
}

static struct buf *
swap_bread(int disk, int disk_block)
{
  return bread(ROOTDEV, swap_phys_block(disk, disk_block));
}

static void
raid1_map(int logical_block, int *disk1, int *disk2, int *disk_block)
{
  int pair = logical_block % (NDISKS / 2);
  *disk1 = pair * 2;
  *disk2 = *disk1 + 1;
  *disk_block = logical_block / (NDISKS / 2);
}

static void
raid5_map(int logical_block, int *data_disk, int *parity_disk, int *disk_block)
{
  int stripe = logical_block / (NDISKS - 1);
  int data_index = logical_block % (NDISKS - 1);

  *parity_disk = stripe % NDISKS;
  *disk_block = stripe;
  *data_disk = data_index;
  if(*data_disk >= *parity_disk)
    (*data_disk)++;
}

// Forward declarations for bread/bwrite/brelse (defined in bio.c).
extern struct buf *bread(uint dev, uint blockno);
extern void        bwrite(struct buf *b);
extern void        brelse(struct buf *b);
extern int         current_disk_head; // maintained by bio.c

// =====================================================================
// swap_disk_read — read one swapped page (PGSIZE bytes) from disk.
// Applies RAID mapping according to the global raid_mode.
// Returns 0 on success, -1 on error.
// =====================================================================
int swap_disk_read(int swap_slot, char *dst)
{
  for(int i = 0; i < BLOCKS_PER_PAGE; i++)
  {
    int logical_block = swap_slot * BLOCKS_PER_PAGE + i;
    int failed_disk = current_failed_disk();

    // ==================== RAID 0 ====================
    if(raid_mode == RAID0)
    {
      int disk       = logical_block % NDISKS;
      int disk_block = logical_block / NDISKS;

      struct buf *b = swap_bread(disk, disk_block);
      memmove(dst + i * BSIZE, b->data, BSIZE);
      brelse(b);
    }

    // ==================== RAID 1 ====================
    // FIX (bug #8): Use per-disk block number (logical / 2 for 2 mirrors),
    // not the raw logical_block, when calling bread for RAID 1.
    // The old code passed logical_block as the blockno, which pointed to
    // completely wrong disk sectors on the mirror disk.
    else if(raid_mode == RAID1)
    {
      int d1, d2, disk_block;
      raid1_map(logical_block, &d1, &d2, &disk_block);

      // If d1 is failed, fall back to d2.
      int use_disk = (d1 == failed_disk) ? d2 : d1;

      struct buf *b = swap_bread(use_disk, disk_block);
      memmove(dst + i * BSIZE, b->data, BSIZE);
      brelse(b);
    }

    // ==================== RAID 5 ====================
    // FIX (bug #6): Restructured to correctly separate the two cases:
    //   No failure  → directly read the appropriate data disk.
    //   One failure → reconstruct via XOR of all surviving disks.
    // The old code unconditionally XOR'd all disks first, then
    // overwrote the result with a direct read for the no-failure case,
    // making the XOR pointless and leaving reconstruction dangling.
    else // RAID5
    {
      // Simple consistent RAID 5 mapping:
      //   stripe_num  = logical_block / (NDISKS-1)  — which stripe row
      //   parity_disk = stripe_num % NDISKS          — spec: parity rotates
      //   disk_block  = logical_block / NDISKS       — physical block on each disk
      //   data_slot   = logical_block % NDISKS       — slot within stripe (0..3)
      //
      // We treat each group of NDISKS logical blocks as one stripe.
      // One of the NDISKS slots holds parity; the rest hold data.
      // parity_disk rotates per stripe so load is balanced.
      // A logical block maps to the data_slot-th physical slot,
      // skipping over whichever slot is the parity disk.
      int data_disk, parity_disk, disk_block;
      raid5_map(logical_block, &data_disk, &parity_disk, &disk_block);

      if(failed_disk == -1)
      {
        // Normal: read directly from the data disk.
        struct buf *b = swap_bread(data_disk, disk_block);
        memmove(dst + i * BSIZE, b->data, BSIZE);
        brelse(b);
      }
      else if(failed_disk != data_disk)
      {
        // Requested data block is still present; only reconstruct if the
        // requested data disk itself has failed.
        struct buf *b = swap_bread(data_disk, disk_block);
        memmove(dst + i * BSIZE, b->data, BSIZE);
        brelse(b);
      }
      else
      {
        // Degraded: reconstruct via XOR of all surviving disks.
        memset(raid5_recovered, 0, BSIZE);
        for(int d = 0; d < NDISKS; d++)
        {
          if(d == failed_disk)
            continue;
          struct buf *b = swap_bread(d, disk_block);
          for(int j = 0; j < BSIZE; j++)
            raid5_recovered[j] ^= b->data[j];
          brelse(b);
        }
        memmove(dst + i * BSIZE, raid5_recovered, BSIZE);
      }
    }
  }

  return 0;
}

// =====================================================================
// swap_disk_write — write one swapped page (PGSIZE bytes) to disk.
// Applies RAID mapping according to the global raid_mode.
// Returns 0 on success, -1 on error.
// =====================================================================
int swap_disk_write(int swap_slot, char *src)
{
  for(int i = 0; i < BLOCKS_PER_PAGE; i++)
  {
    int logical_block = swap_slot * BLOCKS_PER_PAGE + i;
    int failed_disk = current_failed_disk();

    // ==================== RAID 0 ====================
    if(raid_mode == RAID0)
    {
      int disk       = logical_block % NDISKS;
      int disk_block = logical_block / NDISKS;

      struct buf *b = swap_bread(disk, disk_block);
      memmove(b->data, src + i * BSIZE, BSIZE);
      bwrite(b);
      brelse(b);
    }

    // ==================== RAID 1 ====================
    // FIX (bug #8): Use disk_block = logical_block / NDISKS for both
    // mirrors so they map to the same per-disk offset.
    else if(raid_mode == RAID1)
    {
      int d1, d2, disk_block;
      raid1_map(logical_block, &d1, &d2, &disk_block);

      if(d1 != failed_disk){
        struct buf *b1 = swap_bread(d1, disk_block);
        memmove(b1->data, src + i * BSIZE, BSIZE);
        bwrite(b1);
        brelse(b1);
      }

      if(d2 != failed_disk){
        struct buf *b2 = swap_bread(d2, disk_block);
        memmove(b2->data, src + i * BSIZE, BSIZE);
        bwrite(b2);
        brelse(b2);
      }
    }

    // ==================== RAID 5 ====================
    // FIX (bug #7): Parity must be the XOR of ALL data blocks in the
    // stripe, not just the new block being written.
    // The old code computed: parity = 0 XOR new_data = new_data (wrong).
    // Correct approach: read the current contents of every data disk in
    // the stripe, XOR them together with the new data to get the new
    // parity, then write both the data block and the updated parity.
    //
    // For a single-block write in a 4-disk stripe:
    //   new_parity = old_parity XOR old_data XOR new_data
    // (read-modify-write parity update).
    else // RAID5
    {
      // Identical mapping to swap_disk_read — must stay in sync.
      int data_disk, parity_disk, disk_block;
      raid5_map(logical_block, &data_disk, &parity_disk, &disk_block);

      if(failed_disk == -1 || (failed_disk != data_disk && failed_disk != parity_disk))
      {
        struct buf *bd_old = swap_bread(data_disk, disk_block);
        memmove(raid5_old_data, bd_old->data, BSIZE);
        brelse(bd_old);

        struct buf *bp_old = swap_bread(parity_disk, disk_block);
        memmove(raid5_old_parity, bp_old->data, BSIZE);
        brelse(bp_old);

        for(int j = 0; j < BSIZE; j++)
          raid5_new_parity[j] = raid5_old_parity[j] ^ raid5_old_data[j] ^ src[i * BSIZE + j];

        struct buf *bd_new = swap_bread(data_disk, disk_block);
        memmove(bd_new->data, src + i * BSIZE, BSIZE);
        bwrite(bd_new);
        brelse(bd_new);

        struct buf *bp_new = swap_bread(parity_disk, disk_block);
        memmove(bp_new->data, raid5_new_parity, BSIZE);
        bwrite(bp_new);
        brelse(bp_new);
      }
      else if(failed_disk == parity_disk)
      {
        struct buf *bd_new = swap_bread(data_disk, disk_block);
        memmove(bd_new->data, src + i * BSIZE, BSIZE);
        bwrite(bd_new);
        brelse(bd_new);
      }
      else
      {
        memset(raid5_old_data, 0, BSIZE);
        for(int d = 0; d < NDISKS; d++)
        {
          if(d == failed_disk)
            continue;
          struct buf *b = swap_bread(d, disk_block);
          for(int j = 0; j < BSIZE; j++)
            raid5_old_data[j] ^= b->data[j];
          brelse(b);
        }

        struct buf *bp_old = swap_bread(parity_disk, disk_block);
        memmove(raid5_old_parity, bp_old->data, BSIZE);
        brelse(bp_old);

        for(int j = 0; j < BSIZE; j++)
          raid5_new_parity[j] = raid5_old_parity[j] ^ raid5_old_data[j] ^ src[i * BSIZE + j];

        struct buf *bp_new = swap_bread(parity_disk, disk_block);
        memmove(bp_new->data, raid5_new_parity, BSIZE);
        bwrite(bp_new);
        brelse(bp_new);
      }
    }
  }

  return 0;
}

// =====================================================================
// virtio hardware driver — unchanged from xv6 baseline below this line.
// =====================================================================

// the address of virtio mmio register r.
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

static struct disk
{
  struct virtq_desc  *desc;
  struct virtq_avail *avail;
  struct virtq_used  *used;

  char     free[NUM];
  uint16   used_idx;

  struct {
    struct buf *b;
    char        status;
  } info[NUM];

  struct virtio_blk_req ops[NUM];

  struct spinlock vdisk_lock;

} disk;

void virtio_disk_init(void)
{
  uint32 status = 0;

  initlock(&disk.vdisk_lock, "virtio_disk");

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION)      != 2          ||
     *R(VIRTIO_MMIO_DEVICE_ID)    != 2          ||
     *R(VIRTIO_MMIO_VENDOR_ID)    != 0x554d4551)
  {
    panic("could not find virtio disk");
  }

  *R(VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");

  disk.desc  = kalloc();
  disk.avail = kalloc();
  disk.used  = kalloc();
  if(!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc,  0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used,  0, PGSIZE);

  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  *R(VIRTIO_MMIO_QUEUE_DESC_LOW)   = (uint64)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH)  = (uint64)disk.desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW)  = (uint64)disk.avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW)  = (uint64)disk.used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;
}

static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

static void
free_desc(int i)
{
  if(i >= NUM)          panic("free_desc 1");
  if(disk.free[i])      panic("free_desc 2");
  disk.desc[i].addr  = 0;
  disk.desc[i].len   = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next  = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt  = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

void virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / 512);

  acquire(&disk.vdisk_lock);

  int idx[3];
  while(1){
    if(alloc3_desc(idx) == 0)
      break;
    sleep(&disk.free[0], &disk.vdisk_lock);
  }

  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

  if(write)
    buf0->type = VIRTIO_BLK_T_OUT;
  else
    buf0->type = VIRTIO_BLK_T_IN;
  buf0->reserved = 0;
  buf0->sector   = sector;

  disk.desc[idx[0]].addr  = (uint64)buf0;
  disk.desc[idx[0]].len   = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next  = idx[1];

  disk.desc[idx[1]].addr = (uint64)b->data;
  disk.desc[idx[1]].len  = BSIZE;
  if(write)
    disk.desc[idx[1]].flags = 0;
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE;
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next   = idx[2];

  disk.info[idx[0]].status  = 0xff;
  disk.desc[idx[2]].addr    = (uint64)&disk.info[idx[0]].status;
  disk.desc[idx[2]].len     = 1;
  disk.desc[idx[2]].flags   = VRING_DESC_F_WRITE;
  disk.desc[idx[2]].next    = 0;

  b->disk = 1;
  disk.info[idx[0]].b = b;

  disk.avail->ring[disk.avail->idx % NUM] = idx[0];

  __sync_synchronize();

  disk.avail->idx += 1;

  __sync_synchronize();

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

  while(b->disk == 1)
    sleep(b, &disk.vdisk_lock);

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  release(&disk.vdisk_lock);
}

void virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  while(disk.used_idx != disk.used->idx){
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *b = disk.info[id].b;
    b->disk = 0;
    wakeup(b);

    disk.used_idx += 1;
  }

  release(&disk.vdisk_lock);
}
