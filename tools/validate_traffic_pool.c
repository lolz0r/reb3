#include "burnout3_traffic_pool.h"

#include <stdio.h>

static int expect_pair(B3TrafficPool* pool, int physical, int agent) {
    int got_physical = -1;
    int got_agent = -1;
    return b3_traffic_pool_acquire(pool, &got_physical, &got_agent)
        && got_physical == physical && got_agent == agent;
}

int main(void) {
    B3TrafficPool pool;
    int physical, agent;

    b3_traffic_pool_init(&pool, 3, 3);
    if (!expect_pair(&pool, 0, 0) || !expect_pair(&pool, 1, 1)) return 1;
    if (!b3_traffic_pool_release(&pool, 0, 0)) return 1;
    if (!expect_pair(&pool, 2, 0)) return 1;
    if (!b3_traffic_pool_release(&pool, 1, 1)
        || !b3_traffic_pool_release(&pool, 2, 0)) return 1;
    if (!expect_pair(&pool, 0, 0) || !expect_pair(&pool, 1, 1)
        || !expect_pair(&pool, 2, 2)) return 1;
    if (b3_traffic_pool_acquire(&pool, &physical, &agent)) return 1;
    if (!b3_traffic_pool_release(&pool, 0, 0)
        || b3_traffic_pool_release(&pool, 0, 0)) return 1;

    puts("traffic physical FIFO and road-agent LIFO pool lifecycle: OK");
    return 0;
}
