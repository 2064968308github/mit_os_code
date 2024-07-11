#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock locks[NCPU];
  struct run *freelist_CPU[NCPU];
} kmem;

void
kinit()
{
  char lockname[8];
  for (int i = 0; i < NCPU; ++i) {
    snprintf(lockname, sizeof(lockname), "kmem_%d", i);
    initlock(&kmem.locks[i], lockname);    
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc(). (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  push_off();
  int cur_cpu = cpuid();
  acquire(&kmem.locks[cur_cpu]);
  r->next = kmem.freelist_CPU[cur_cpu];
  kmem.freelist_CPU[cur_cpu] = r;
  release(&kmem.locks[cur_cpu]);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  int cur_cpu = cpuid();

  acquire(&kmem.locks[cur_cpu]);
  r = kmem.freelist_CPU[cur_cpu];
  if (r) {
    kmem.freelist_CPU[cur_cpu] = r->next;
  }
  release(&kmem.locks[cur_cpu]);

  if (!r) {
    for (int i = 0; i < NCPU; ++i) {
      if (i == cur_cpu) continue;

      acquire(&kmem.locks[i]);
      r = kmem.freelist_CPU[i];
      if (r) {
        kmem.freelist_CPU[i] = r->next;
        release(&kmem.locks[i]);
        break;
      }
      release(&kmem.locks[i]);
    }
  }

  pop_off();

  if (r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
