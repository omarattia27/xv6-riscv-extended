#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define COW_CAUSE 15

extern int refcount[PHYSTOP/PGSIZE];
extern struct spinlock refcount_lock; 

// Debug counters
int cow_faults_copy = 0;    // COW faults that required copying
int cow_faults_promote = 0; // COW faults with refcount=1 (just make writable)
int lazy_alloc_faults = 0;  // Lazy allocation faults

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
uint64
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);  //DOC: kernelvec

  struct proc *p = myproc();
  struct thread *t = mythread();
  
  // Sanity check: we should always have a valid thread in usertrap
  if(t == 0)
    panic("usertrap: no thread");
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(killed_thread(t))
      kexit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if(r_scause() == 15){  // handle copy-on-write page fault
    uint64 va = r_stval();
    
    // Check if address is valid (reject page 0 for null pointer safety)
    if(va < PGSIZE || va >= p->sz || va >= MAXVA) {
      printf("COW page fault: invalid address 0x%lx (sz=0x%lx)\n", va, p->sz);
      setkilled_thread(t);
      goto killed;
    }
    
    // Check if this is a COW page (valid, read-only, user page)
    pte_t *pte = walk(p->pagetable, va, 0);

    if(pte && (*pte & PTE_V) && !(*pte & PTE_W) && (*pte & PTE_U)) {
      // This is a COW page fault
      uint64 pa = PTE2PA(*pte);
      
      acquire(&refcount_lock);
      int refs = refcount[pa/PGSIZE];
      release(&refcount_lock);
      
      if(refs > 1) {
        // allocate new page
        uint64 mem = (uint64) kalloc();
        if(mem == 0) {
          setkilled_thread(t);
          return 0;
        }
        // copy data from old page to new page
        memmove((void*)mem, (void*)pa, PGSIZE);
        // update PTE to point to new page
        *pte = PA2PTE(mem) | PTE_FLAGS(*pte) | PTE_W;
        // decrement refcount of old page
        acquire(&refcount_lock);
        refcount[pa/PGSIZE]--;
        release(&refcount_lock);
        cow_faults_copy++;
        // printf("[COW] pid=%d copied page (refcount was %d) total_copy=%d\n", p->pid, refs, cow_faults_copy);
      } else {
        // only one reference, can just make it writable
        *pte |= PTE_W;
        cow_faults_promote++;
        // printf("[COW] pid=%d promoted page to writable (refcount=1) total_promote=%d\n", p->pid, cow_faults_promote);
      }
      sfence_vma();
    } else if(vmfault(p->pagetable, r_stval(), 0) == 0) {
      // Not a COW page, and vmfault failed - can't handle this
      printf("usertrap(): unhandled page fault scause 0x%lx pid=%d\n", r_scause(), p->pid);
      printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
      setkilled_thread(t);
    } else {
      // vmfault succeeded, lazy allocation handled it
      lazy_alloc_faults++;
      // printf("[LAZY] pid=%d allocated page on demand, total=%d\n", p->pid, lazy_alloc_faults);
    }
  } else if(r_scause() == 13) {
    // Load page fault - try lazy allocation
    if(vmfault(p->pagetable, r_stval(), 1) == 0) {
      // vmfault failed - invalid address
      printf("usertrap(): load page fault, invalid address scause=0x%lx pid=%d\n", r_scause(), p->pid);
      printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
      setkilled_thread(t);
    }
  } else if(r_scause() == 12) {
    // Instruction page fault - try lazy allocation
    if(vmfault(p->pagetable, r_stval(), 1) == 0) {
      // vmfault failed - invalid address  
      printf("usertrap(): instruction page fault, invalid address scause=0x%lx pid=%d\n", r_scause(), p->pid);
      printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
      setkilled_thread(t);
    }
  }
  else{
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled_thread(t);
  }

killed:
  if(killed_thread(t))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  prepare_return();

  // the user page table to switch to, for trampoline.S
  uint64 satp = MAKE_SATP(p->pagetable);

  // return to trampoline.S; satp value in a0.
  return satp;
}

//
// set up trapframe and control registers for a return to user space
//
void
prepare_return(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(). because a trap from kernel
  // code to usertrap would be a disaster, turn off interrupts.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // Set sscratch to point to this thread's trapframe address
  // The trampoline will use this to save/restore registers
  w_sscratch((uint64) TRAPFRAME);

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // Not a device interrupt - check if it's a page fault we can handle
    if(scause == 13 || scause == 15) {
      // Load page fault (13) or Store/AMO page fault (15)
      // These shouldn't happen in kernel mode - kernel addresses should always be mapped
      printf("Kernel page fault: scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, sepc, r_stval());
      struct proc *p = myproc();
      struct thread *t = mythread();
      printf("myproc()=%p mythread()=%p\n", p, t);
      if(t) {
        printf("thread: state=%d tid=%d kstack=0x%lx\n", t->state, t->tid, t->kstack);
      }
      panic("kernel page fault");
    }
    
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    
    // Try to get some context
    struct proc *p = myproc();
    struct thread *t = mythread();
    printf("myproc()=%p mythread()=%p\n", p, t);
    if(t) {
      printf("thread state=%d tid=%d\n", t->state, t->tid);
    }
    
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  struct thread *t = mythread();
  if(which_dev == 2 && t != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

