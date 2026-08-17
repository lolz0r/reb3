#ifndef BURNOUT3_TRAFFIC_RESERVATIONS_H
#define BURNOUT3_TRAFFIC_RESERVATIONS_H

/* Upper bound on agents considered per path by the owner-map sweep's
 * scratch array.  Above this the rebuild falls back to the original
 * exhaustive scan, so the result is identical either way. */
#define B3_TRAFFIC_RES_MAX_AGENTS 256

typedef struct {
    unsigned int pair_base;
    unsigned int count;
} B3TrafficReservationPath;

typedef struct {
    unsigned short path_id;
    float cursor;
    int active;
    int ahead;
    int behind;
} B3TrafficReservationAgent;

void b3_traffic_reservations_rebuild(
    const B3TrafficReservationPath* paths, unsigned int path_count,
    B3TrafficReservationAgent* agents, int agent_count,
    int* owners, unsigned int owner_count);

#endif
