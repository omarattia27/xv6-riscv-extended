// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
  for(int i = 0; i < 4; i++)
    lk->last_acquire_stack[i] = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock *lk)
{
  if(lk == 0) {
    struct proc *p = myproc();
    struct thread *t = mythread();
    printf("acquire: NULL lock! myproc()=%p mythread()=%p\n", p, t);
    if(t) {
      printf("thread: state=%d tid=%d magic=0x%lx\n", t->state, t->tid, t->magic);
      printf("thread lock addr: &t->lock=%p\n", &t->lock);
      printf("killed_thread debug: t=%p lock=%p lock.name=%s lock.locked=%d\n", 
             t, &t->lock, t->lock.name ? t->lock.name : "NULL", t->lock.locked);
    }
    
    // Try to get the return address to see who called us
    uint64 ra;
    asm volatile("mv %0, ra" : "=r" (ra));
    printf("acquire called from ra=0x%lx\n", ra);
    
    panic("acquire: NULL lock");
  }
  
  push_off(); // disable interrupts to avoid deadlock.
  if(holding(lk)) {
    struct proc *p = myproc();
    struct thread *t = mythread();
    printf("acquire: already holding lock %s\n", lk->name);
    printf("acquire double-lock: myproc()=%p mythread()=%p\n", p, t);
    printf("Lock acquired call stack:\n");
    for(int i = 0; i < 4; i++) {
      if(lk->last_acquire_stack[i] != 0) {
        printf("  [%d] ra=0x%lx\n", i, lk->last_acquire_stack[i]);
      }
    }
    
    // Current call stack
    uint64 current_fp, current_ra;
    asm volatile("mv %0, s0" : "=r" (current_fp));
    printf("Current call stack:\n");
    for(int i = 0; i < 4 && current_fp != 0; i++) {
      if(current_fp < 0x80000000 || current_fp > 0x90000000) break;
      current_ra = *(uint64*)(current_fp - 8);
      printf("  [%d] ra=0x%lx\n", i, current_ra);
      current_fp = *(uint64*)(current_fp - 16);
    }
    
    if(t) {
      printf("thread: state=%d tid=%d magic=0x%lx\n", t->state, t->tid, t->magic);
      printf("thread lock addr: &t->lock=%p\n", &t->lock);
    }
    
    panic("acquire");
  }

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
  
  // Capture call stack for debugging
  uint64 fp, ra;
  asm volatile("mv %0, s0" : "=r" (fp));  // Get frame pointer
  
  // Walk the call stack to get multiple return addresses
  for(int i = 0; i < 4 && fp != 0; i++) {
    if(fp < 0x80000000 || fp > 0x90000000) break; // Sanity check
    ra = *(uint64*)(fp - 8);  // Return address is at fp-8
    lk->last_acquire_stack[i] = ra;
    fp = *(uint64*)(fp - 16); // Previous frame pointer is at fp-16
  }
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if(lk == 0)
    panic("release: NULL lock");
  
  if(!holding(lk)) {
    struct cpu *current = mycpu();
    printf("release: not holding lock!\n");
    printf("  lk=%p locked=%d cpu=%p mycpu=%p\n", 
           lk, lk->locked, lk->cpu, current);
    printf("  lock name: %s\n", lk->name);
    printf("  cpuid: current=%d lock's cpu id=%d\n", 
           cpuid(), lk->cpu ? (int)(lk->cpu - cpus) : -1);
    printf("  holding check: locked=%d cpu_match=%d\n", 
           lk->locked, lk->cpu == current);
    panic("release");
  }

  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct spinlock *lk)
{
  int r;
  
  // Critical: Check if we're being called with a corrupted lock pointer
  if(lk == 0 || (uint64)lk < 0x80000000) {
    printf("holding: invalid lock pointer lk=%p\n", lk);
    printf("This may indicate stack or process corruption\n");
    
    // Try to get current cpu info safely
    int hart_id = r_tp();
    if(hart_id >= 0 && hart_id < NCPU) {
      printf("hart_id=%d seems valid\n", hart_id);
      struct cpu *c = &cpus[hart_id];
      printf("cpu=%p proc=%p thread=%p\n", c, c->proc, c->thread);
    } else {
      printf("hart_id=%d is invalid!\n", hart_id);
    }
    return 0;  // Safe return to avoid panic in exit path
  }
  
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  int old = intr_get();

  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  intr_off();

  if(mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  // Early corruption detection
  int hart_id = r_tp();
  if(hart_id < 0 || hart_id >= NCPU) {
    printf("pop_off: CORRUPTION DETECTED! tp=%d (should be 0-%d)\n", hart_id, NCPU-1);
    printf("This will cause a page fault when accessing cpus[%d]\n", hart_id);
    panic("pop_off: tp register corrupted");
  }
  
  struct cpu *c = mycpu();
  
  // Validate cpu structure
  if(c == 0 || (uint64)c < 0x80000000) {
    printf("pop_off: Invalid cpu pointer: %p\n", c);
    printf("hart_id=%d cpus array at %p\n", hart_id, &cpus[0]);
    panic("pop_off: invalid cpu pointer");
  }
  
  if(intr_get())
    panic("pop_off - interruptible");
  if(c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if(c->noff == 0 && c->intena)
    intr_on();
}
