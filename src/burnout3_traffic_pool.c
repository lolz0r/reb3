#include "burnout3_traffic_pool.h"

#include <string.h>

static int b3_traffic_pool_valid_physical(const B3TrafficPool* pool, int slot) {
    return pool && slot >= 0 && slot < pool->physical_count;
}

static int b3_traffic_pool_valid_agent(const B3TrafficPool* pool, int slot) {
    return pool && slot >= 0 && slot < pool->agent_count;
}

void b3_traffic_pool_init(B3TrafficPool* pool, int physical_count,
                          int agent_count) {
    if (!pool) return;
    memset(pool, 0, sizeof(*pool));
    if (physical_count < 0) physical_count = 0;
    if (physical_count > B3_TRAFFIC_POOL_MAX)
        physical_count = B3_TRAFFIC_POOL_MAX;
    if (agent_count < 0) agent_count = 0;
    if (agent_count > B3_TRAFFIC_POOL_MAX) agent_count = B3_TRAFFIC_POOL_MAX;
    pool->physical_count = physical_count;
    pool->agent_count = agent_count;
    pool->physical_head = physical_count ? 0 : -1;
    pool->physical_tail = physical_count ? physical_count - 1 : -1;
    pool->agent_head = agent_count ? 0 : -1;
    for (int slot = 0; slot < physical_count; slot++)
        pool->physical_next[slot] = slot + 1 < physical_count ? slot + 1 : -1;
    for (int slot = 0; slot < agent_count; slot++)
        pool->agent_next[slot] = slot + 1 < agent_count ? slot + 1 : -1;
}

int b3_traffic_pool_acquire(B3TrafficPool* pool, int* physical_slot,
                            int* agent_slot) {
    int physical;
    int agent;
    if (!pool || !physical_slot || !agent_slot || pool->physical_head < 0
        || pool->agent_head < 0)
        return 0;
    physical = pool->physical_head;
    agent = pool->agent_head;
    pool->physical_head = pool->physical_next[physical];
    if (pool->physical_head < 0) pool->physical_tail = -1;
    pool->physical_next[physical] = -1;
    pool->agent_head = pool->agent_next[agent];
    pool->agent_next[agent] = -1;
    pool->physical_live[physical] = 1;
    pool->agent_live[agent] = 1;
    *physical_slot = physical;
    *agent_slot = agent;
    return 1;
}

int b3_traffic_pool_release(B3TrafficPool* pool, int physical_slot,
                            int agent_slot) {
    if (!b3_traffic_pool_valid_physical(pool, physical_slot)
        || !b3_traffic_pool_valid_agent(pool, agent_slot)
        || !pool->physical_live[physical_slot] || !pool->agent_live[agent_slot])
        return 0;
    pool->physical_live[physical_slot] = 0;
    pool->agent_live[agent_slot] = 0;
    pool->physical_next[physical_slot] = -1;
    if (pool->physical_tail < 0) pool->physical_head = physical_slot;
    else pool->physical_next[pool->physical_tail] = physical_slot;
    pool->physical_tail = physical_slot;
    pool->agent_next[agent_slot] = pool->agent_head;
    pool->agent_head = agent_slot;
    return 1;
}
