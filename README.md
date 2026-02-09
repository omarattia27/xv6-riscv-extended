# xv6 Extended Kernel

This repository contains extensions to the xv6 teaching operating system focused on **kernel-level multithreading, scheduling policies, and virtual memory behavior**.

The goal of this project is to explore **core OS execution paths**, understand design tradeoffs, and extend xv6 while preserving its simplicity and debuggability.

---

## Overview

Key areas of extension include:

- Kernel-level multithreading
- Multi-Level Feedback Queue (MLFQ) scheduler
- Copy-on-write (COW) virtual memory
- Conceptual NUMA-aware memory considerations

This project prioritizes **correctness, clarity, and architectural understanding** over feature completeness.

---

## Kernel-Level Multithreading (Core Feature)

xv6 was extended to support **multiple threads per process** (with a maximum of three threads per process), enabling concurrent execution within a shared address space.

### Thread Model
- Threads **share**:
  - address space (page table)
  - file descriptors
- Threads have:
  - independent kernel stacks
  - independent execution contexts

A process acts as a container for one or more threads.

---

### Thread Lifecycle

Supported operations
- `thread_create`
- `thread_join`
- `thread_exit`

Lifecycle handling ensures:
- safe kernel stack allocation and teardown
- correct synchronization between exiting and joining threads
- proper cleanup of thread resources

---

### Context Switching & Trap Handling

Context switching and trap handling paths were modified to:
- switch execution at the **thread level** rather than the process level used in the original xv6 implementation
- preserve process-wide state
- safely transition between user and kernel mode

---

## Scheduling: Multi-Level Feedback Queue (MLFQ)

The default xv6 scheduler was extended to implement a **Multi-Level Feedback Queue (MLFQ)** policy to improve fairness and responsiveness under multithreaded workloads.

Key characteristics:
- multiple priority queues
- dynamic priority adjustment based on execution behavior
- preemption based on queue level

Threads are scheduled as independent runnable entities while remaining logically associated with their parent process.

---

## Virtual Memory: Copy-on-Write (COW)

To improve memory efficiency and process creation performance, **copy-on-write (COW)** semantics were added.

Design overview:
- pages are initially shared and marked read-only
- write attempts trigger a page fault
- the fault handler allocates and copies the page on demand

This required modifications to page fault handling and page table updates, as well as the introduction of a new data structure to track the number of active references to each physical page.

---

## NUMA-Aware Concepts (Analytical)

While xv6 does not provide hardware-level NUMA support, this project explores **NUMA-aware design considerations**, including:
- memory locality
- thread placement implications
- scalability tradeoffs in multiprocessor systems

This extension is **conceptual** and focuses on design reasoning rather than physical enforcement.

---

## Testing & Validation

Testing focused on:
- correct thread creation and termination
- scheduler fairness under mixed workloads
- correct COW behavior under concurrent writes
- kernel stability under stress scenarios

Debugging relied on kernel logging and controlled test workloads.

---

## Design Philosophy

- Preserve xv6 simplicity
- Minimize invasive kernel changes
- Prioritize correctness over performance
- Favor clarity and debuggability

---

## Challenges and Lessons Learned

- Adding deeper and more invasive features required extensive **GDB debugging**, particularly when diagnosing concurrency-related bugs.
- Working with xv6 required careful reading of the documentation and reasoning about **strict kernel invariants** before writing code.
- During the multithreading implementation, deep debugging was necessary to verify kernel stack placement, address correctness of core components, and ensure that the shared process address space was not corrupted.

---

## Limitations

- Simplified synchronization primitives
- No hardware-level NUMA enforcement
- Limited scalability due to xv6 constraints

These limitations are intentional.

---

## Motivation

This project was built to gain hands-on experience with:
- OS execution models
- scheduling tradeoffs
- virtual memory mechanisms
- kernel design constraints

It serves as a practical exploration of operating system internals beyond the base xv6 implementation.


---

xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix
Version 6 (v6).  xv6 loosely follows the structure and style of v6,
but is implemented for a modern RISC-V multiprocessor using ANSI C.

ACKNOWLEDGMENTS

xv6 is inspired by John Lions's Commentary on UNIX 6th Edition (Peer
to Peer Communications; ISBN: 1-57398-013-7; 1st edition (June 14,
2000)).  See also https://pdos.csail.mit.edu/6.1810/, which provides
pointers to on-line resources for v6.

The following people have made contributions: Russ Cox (context switching,
locking), Cliff Frey (MP), Xiao Yu (MP), Nickolai Zeldovich, and Austin
Clements.

We are also grateful for the bug reports and patches contributed by
Abhinavpatel00, Takahiro Aoyagi, Marcelo Arroyo, Hirbod Behnam, Silas
Boyd-Wickizer, Anton Burtsev, carlclone, Ian Chen, clivezeng, Dan
Cross, Cody Cutler, Mike CAT, Tej Chajed, Asami Doi,Wenyang Duan,
echtwerner, eyalz800, Nelson Elhage, Saar Ettinger, Alice Ferrazzi,
Nathaniel Filardo, flespark, Peter Froehlich, Yakir Goaron, Shivam
Handa, Matt Harvey, Bryan Henry, jaichenhengjie, Jim Huang, Matúš
Jókay, John Jolly, Alexander Kapshuk, Anders Kaseorg, kehao95,
Wolfgang Keller, Jungwoo Kim, Jonathan Kimmitt, Eddie Kohler, Vadim
Kolontsov, Austin Liew, l0stman, Pavan Maddamsetti, Imbar Marinescu,
Yandong Mao, Matan Shabtay, Hitoshi Mitake, Carmi Merimovich,
mes900903, Mark Morrissey, mtasm, Joel Nider, Hayato Ohhashi,
OptimisticSide, papparapa, phosphagos, Harry Porter, Greg Price, Zheng
qhuo, Quancheng, RayAndrew, Jude Rich, segfault, Ayan Shafqat, Eldar
Sehayek, Yongming Shen, Fumiya Shigemitsu, snoire, Taojie, Cam Tenny,
tyfkda, Warren Toomey, Stephen Tu, Alissa Tung, Rafael Ubal, unicornx,
Amane Uehara, Pablo Ventura, Luc Videau, Xi Wang, WaheedHafez, Keiichi
Watanabe, Lucas Wolf, Nicolas Wolovick, wxdao, Grant Wu, x653, Andy
Zhang, Jindong Zhang, Icenowy Zheng, ZhUyU1997, and Zou Chang Wei.

ERROR REPORTS

Please send errors and suggestions to Frans Kaashoek and Robert Morris
(kaashoek,rtm@mit.edu).  The main purpose of xv6 is as a teaching
operating system for MIT's 6.1810, so we are more interested in
simplifications and clarifications than new features.

BUILDING AND RUNNING XV6

You will need a RISC-V "newlib" tool chain from
https://github.com/riscv/riscv-gnu-toolchain, and qemu compiled for
riscv64-softmmu.  Once they are installed, and in your shell
search path, you can run "make qemu".
