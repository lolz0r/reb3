#ifndef BURNOUT3_TRAFFIC_POOL_H
#define BURNOUT3_TRAFFIC_POOL_H

#define B3_TRAFFIC_POOL_MAX 64

typedef struct {
    int physical_head;
    int physical_tail;
    int agent_head;
    int physical_next[B3_TRAFFIC_POOL_MAX];
    int agent_next[B3_TRAFFIC_POOL_MAX];
    unsigned char physical_live[B3_TRAFFIC_POOL_MAX];
    unsigned char agent_live[B3_TRAFFIC_POOL_MAX];
    int physical_count;
    int agent_count;
} B3TrafficPool;

void b3_traffic_pool_init(B3TrafficPool* pool, int physical_count,
                          int agent_count);
int b3_traffic_pool_acquire(B3TrafficPool* pool, int* physical_slot,
                            int* agent_slot);
int b3_traffic_pool_release(B3TrafficPool* pool, int physical_slot,
                            int agent_slot);

#endif
