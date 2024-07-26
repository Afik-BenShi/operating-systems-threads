#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <stdatomic.h>

#include "queue.h"

typedef struct Node
{
    void *data;
    struct Node *next;
} Node;

typedef struct
{
    Node *head;
    Node *tail;
    size_t size;
    mtx_t mutex; // data lock
} DataQueue;

typedef struct Thread
{
    cnd_t wake;
    struct Thread *next;
} Thread;

typedef struct
{
    Thread *head;
    Thread *tail;
    size_t waiting;
    mtx_t mutex; // to prevent deadlocks, should only be accessed within a data lock
} ThreadQueue;

DataQueue data_q;
ThreadQueue threads_q;
atomic_size_t _visited; // thread safe without using locks

void initQueue(void)
{
    data_q.head = NULL;
    data_q.tail = NULL;
    data_q.size = 0;
    mtx_init(&data_q.mutex, mtx_plain);

    threads_q.head = NULL;
    threads_q.tail = NULL;
    threads_q.waiting = 0;
    mtx_init(&threads_q.mutex, mtx_plain);

    atomic_init(&_visited, 0);
}
void destroyQueue(void)
{
    // destroy data queue
    mtx_lock(&data_q.mutex);
    while (data_q.head != NULL)
    {
        Node *head = data_q.head;
        data_q.head = data_q.head->next;
        free(head);
    }
    mtx_unlock(&data_q.mutex);
    mtx_destroy(&data_q.mutex);

    // destroy threads queue
    mtx_lock(&threads_q.mutex);
    while (threads_q.head != NULL)
    {
        Thread *head = threads_q.head;
        threads_q.head = threads_q.head->next;
        cnd_destroy(&head->wake);
        free(head);
    }
    mtx_unlock(&threads_q.mutex);
    mtx_destroy(&threads_q.mutex);
}
void enqueue(void *item)
{
    Node *newNode = malloc(sizeof(Node));
    newNode->data = item;
    newNode->next = NULL;

    // append new node to the queue
    mtx_lock(&data_q.mutex);
    if (data_q.size == 0)
    {
        data_q.head = newNode;
        data_q.tail = newNode;
    }
    else
    {
        data_q.tail->next = newNode;
        data_q.tail = newNode;
    }
    data_q.size++;

    // wake sleeping threads
    mtx_lock(&threads_q.mutex);
    if (threads_q.waiting > 0)
    {
        Thread *head = threads_q.head;
        threads_q.head = head->next;
        threads_q.waiting--;
        cnd_signal(&head->wake);
    }
    mtx_unlock(&threads_q.mutex);
    mtx_unlock(&data_q.mutex); // unlock after threads_q unlocks to avoid deadlocks
}

void *dequeue(void)
{
    mtx_lock(&data_q.mutex);
    mtx_lock(&threads_q.mutex); // lock only inside data lock
    if (data_q.size == 0 || data_q.size <= threads_q.waiting)
    {
        Thread thread; // stack allocated thread, no need to free later
        thread.next = NULL;
        cnd_init(&thread.wake);

        if (threads_q.waiting == 0)
        {
            threads_q.tail = &thread;
            threads_q.head = &thread;
        }
        else
        {
            threads_q.tail->next = &thread;
            threads_q.tail = &thread;
        }

        threads_q.waiting++;
        mtx_unlock(&threads_q.mutex);
        cnd_wait(&thread.wake, &data_q.mutex);
        // item dequeue is done later. For now only clean cnd
        cnd_destroy(&thread.wake);
    }
    else
    {
        // no need to wait, just get the data out
        mtx_unlock(&threads_q.mutex);
    }

    Node *head = data_q.head;
    data_q.head = head->next;
    data_q.size--;
    atomic_fetch_add(&_visited, 1);

    void *item = head->data;
    free(head);
    mtx_unlock(&data_q.mutex);
    return item;
}

bool tryDequeue(void **result)
{
    mtx_lock(&data_q.mutex);
    mtx_lock(&threads_q.mutex);
    if (data_q.size == 0 || data_q.size <= threads_q.waiting)
    {
        // could not dequeue
        mtx_unlock(&data_q.mutex);
        mtx_unlock(&threads_q.mutex);
        return false;
    }

    Node *head = data_q.head;
    data_q.head = head->next;
    data_q.size--;
    atomic_fetch_add(&_visited, 1);

    *result = head->data;
    free(head);
    mtx_unlock(&threads_q.mutex);
    mtx_unlock(&data_q.mutex);
    return true;
}

size_t size(void)
{
    size_t size;
    mtx_lock(&data_q.mutex);
    size = data_q.size;
    mtx_unlock(&data_q.mutex);
    return size;
}

size_t waiting(void)
{
    size_t waiting;
    mtx_lock(&data_q.mutex);
    waiting = data_q.size;
    mtx_unlock(&data_q.mutex);
    return waiting;
}

size_t visited(void)
{
    return atomic_load(&_visited);
}
