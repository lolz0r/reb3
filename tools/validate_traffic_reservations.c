#include "burnout3_traffic_reservations.h"

#include <stdio.h>

int main(void) {
    const B3TrafficReservationPath paths[] = {
        {0, 10}, {10, 4}
    };
    B3TrafficReservationAgent agents[] = {
        {0, 2.3f, 1, 99, 99}, {0, 5.2f, 1, 99, 99},
        {0, 8.8f, 1, 99, 99}, {1, 1.5f, 1, 99, 99},
        {0, 7.0f, 0, 99, 99}
    };
    int owners[14];
    const int expected_owners[] = {
        0, 0, 0, 1, 1, 1, 2, 2, 2, -1, 3, 3, -1, -1
    };

    b3_traffic_reservations_rebuild(paths, 2, agents, 5, owners, 14);
    if (agents[0].ahead != 1 || agents[0].behind != -1
        || agents[1].ahead != 2 || agents[1].behind != 0
        || agents[2].ahead != -1 || agents[2].behind != 1
        || agents[3].ahead != -1 || agents[3].behind != -1
        || agents[4].ahead != -1 || agents[4].behind != -1) {
        fprintf(stderr, "traffic reservation leader chain regression\n");
        return 1;
    }
    for (int owner_index = 0; owner_index < 14; owner_index++) {
        if (owners[owner_index] != expected_owners[owner_index]) {
            fprintf(stderr, "traffic reservation row %d: got %d, expected %d\n",
                    owner_index, owners[owner_index],
                    expected_owners[owner_index]);
            return 1;
        }
    }
    agents[0].cursor = 6.1f;
    b3_traffic_reservations_rebuild(paths, 2, agents, 5, owners, 14);
    if (agents[1].ahead != 0 || agents[0].behind != 1
        || agents[0].ahead != 2 || agents[2].behind != 0
        || owners[5] != 1 || owners[6] != 0 || owners[7] != 2) {
        fprintf(stderr, "traffic cursor-commit reservation regression\n");
        return 1;
    }
    puts("traffic reservation owner map and leader chain: OK");
    return 0;
}
