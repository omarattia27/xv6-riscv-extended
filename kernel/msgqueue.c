#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "msgqueue.h"

#define  MAX_MSGS 10
#define  MAX_MQ   16

struct msgqtable msgqtable;

void init_msgqueue_table(void) {
    initlock(&msgqtable.lock, "msgqtable");
    msgqtable.queues = kalloc();
    for (int i = 0; i < MAX_MQ; i++) {
        msgqtable.queues[i] = 0;
    }
}


int msgget(int id){

  for (int i = 0; i < MAX_MQ; i++) {
    if (msgqtable.queues[i] != 0 && msgqtable.queues[i]->id == id) {
      return i;
    }
  }
  // create new message queue
  for (int i = 0; i < MAX_MQ; i++) {
    if (msgqtable.queues[i] == 0) {
      struct msgqueue* mq = kalloc();
      if (mq == 0) {
        return -1; // allocation failed
      }
      initlock(&mq->lock, "msgqueue");
      mq->head = 0;
      mq->tail = 0;
      mq->id = id;
      mq->n_msgs = 0;
      mq->max_msgs = MAX_MSGS;
      mq->sendopen = 1;
      mq->recvopen = 1;
      mq->send_wait = 0;
      mq->recv_wait = 0;
      msgqtable.queues[i] = mq;
      return i;
    }
  }
  return -1;
}

// Send a message to the queue identified by mqid
// a string msg of size msgsz
int msgsnd(int mqid,  uint64 msg, int msgsz){
    if (mqid < 0 || mqid >= MAX_MQ || msgqtable.queues[mqid] == 0) {
        return -1; // invalid mqid
    }

    struct msgqueue* mq = msgqtable.queues[mqid];
    acquire(&mq->lock);
    if (mq->n_msgs >= mq->max_msgs) {
        release(&mq->lock);
        return -1; // queue full
    }
    
    struct message* m = kalloc();
    if (m == 0) {
        release(&mq->lock);
        return -1; // allocation failed
    }
    m->size = msgsz;
    struct proc *p = myproc();
    if (copyin(p->pagetable, m->data, msg, msgsz) < 0) {
        kfree(m);
        release(&mq->lock);
        return -1;
    }
    m->next = 0;
    if (mq->tail) {
        mq->tail->next = m;
    } else {
        mq->head = m;
    }
    mq->tail = m;
    mq->n_msgs++;
    release(&mq->lock);

    return 0;
}


int msgrcv(int mqid, uint64 msg, int msgsz){
    if (mqid < 0 || mqid >= MAX_MQ || msgqtable.queues[mqid] == 0) {
        return -1; // invalid mqid
    }
    struct msgqueue* mq = msgqtable.queues[mqid];
    acquire(&mq->lock);
    if (mq->n_msgs == 0) {
        release(&mq->lock);
        return -1; // queue empty
    }
    struct message* m = mq->head;
    mq->head = m->next;
    if (mq->head == 0) {
        mq->tail = 0;
    }
    mq->n_msgs--;
    int copy_size = (msgsz < m->size) ? msgsz : m->size;
    struct proc *p = myproc();
    if (copyout(p->pagetable, msg, m->data, copy_size) < 0) {
        kfree(m);
        release(&mq->lock);
        return -1;
    }
    kfree(m);
    release(&mq->lock);
    return copy_size;
}