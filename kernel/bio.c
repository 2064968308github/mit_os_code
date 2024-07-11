// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUCKET_SIZE 13

struct {
  struct spinlock lock[BUCKET_SIZE];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[BUCKET_SIZE]; // 改为结构体实例
} bcache;

void
binit(void)
{
  struct buf *b;
  char lockname[10];

  for(int i = 0; i < BUCKET_SIZE; ++i){
    snprintf(lockname, sizeof(lockname), "bcache_%d", i);
    initlock(&bcache.lock[i], lockname);
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  for(int i = 0; i < NBUF; ++i){
    b = &bcache.buf[i];
    int cur_bucket = i % BUCKET_SIZE;
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.head[cur_bucket].next;
    b->prev = &bcache.head[cur_bucket];
    bcache.head[cur_bucket].next->prev = b;
    bcache.head[cur_bucket].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  const int cur_bucket = blockno % BUCKET_SIZE;

  acquire(&bcache.lock[cur_bucket]);

  // 查找当前hash桶
  for(b = bcache.head[cur_bucket].next; b != &bcache.head[cur_bucket]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      acquire(&tickslock);
      b->used_tick = ticks;
      release(&tickslock);
      release(&bcache.lock[cur_bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  struct buf *temp = 0;
  uint temp_tick = 0;
  for(int i = 0; i < BUCKET_SIZE; ++i){
    int bucket = (cur_bucket + i) % BUCKET_SIZE;
    if(bucket != cur_bucket){
      if(!holding(&bcache.lock[bucket])) acquire(&bcache.lock[bucket]);
      else continue;
    }
    for(b = bcache.head[bucket].next; b != &bcache.head[bucket]; b = b->next){
      if(b->refcnt == 0 && (temp_tick == 0 || b->used_tick < temp_tick)){
        temp = b;
        temp_tick = b->used_tick;  // 立即更新 temp_tick
      }
    }
    if(temp){
      if(bucket != cur_bucket){
        temp->prev->next = temp->next;
        temp->next->prev = temp->prev;
        release(&bcache.lock[bucket]);
        temp->next = bcache.head[cur_bucket].next;
        temp->prev = &bcache.head[cur_bucket];
        bcache.head[cur_bucket].next->prev = temp;
        bcache.head[cur_bucket].next = temp;
      }
      b = temp;
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      acquire(&tickslock);
      b->used_tick = ticks;
      release(&tickslock);
      release(&bcache.lock[cur_bucket]);
      acquiresleep(&b->lock);
      return b;
    } else {
      if(bucket != cur_bucket){
        release(&bcache.lock[bucket]);
      }
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  acquire(&tickslock);
  b->used_tick = ticks;
  release(&tickslock);
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int cur_bucket = b->blockno % BUCKET_SIZE;
  acquire(&bcache.lock[cur_bucket]);
  b->refcnt--;
  acquire(&tickslock);
  b->used_tick = ticks;
  release(&tickslock);

  if(b->refcnt == 0){
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head[cur_bucket].next;
    b->prev = &bcache.head[cur_bucket];
    bcache.head[cur_bucket].next->prev = b;
    bcache.head[cur_bucket].next = b;
  }

  release(&bcache.lock[cur_bucket]);
}

void
bpin(struct buf *b) {
  int cur_bucket = b->blockno % BUCKET_SIZE;
  acquire(&bcache.lock[cur_bucket]);
  b->refcnt++;
  release(&bcache.lock[cur_bucket]);
}

void
bunpin(struct buf *b) {
  int cur_bucket = b->blockno % BUCKET_SIZE;
  acquire(&bcache.lock[cur_bucket]);
  b->refcnt--;
  release(&bcache.lock[cur_bucket]);
}
