#include "spinlock.h"

struct message{
    int size;
    struct message* next;
    char data[];
};

struct msgqueue {
    struct spinlock lock;
    
    struct message* head;
    struct message* tail;
    int id;

    int n_msgs;
    int max_msgs;

    int sendopen;
    int recvopen;

    //
    void *send_wait;
    void *recv_wait;
};

struct msgqtable{
    struct spinlock lock;
    struct msgqueue** queues;
};