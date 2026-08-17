#include "burnout3_traffic_reservations.h"

#include <math.h>

static int b3_traffic_agent_precedes(const B3TrafficReservationAgent* first,
                                     int first_index,
                                     const B3TrafficReservationAgent* second,
                                     int second_index) {
    const float epsilon = 1e-5f;
    if (first->cursor > second->cursor + epsilon) return 1;
    if (second->cursor > first->cursor + epsilon) return 0;
    return first_index > second_index;
}

void b3_traffic_reservations_rebuild(
    const B3TrafficReservationPath* paths, unsigned int path_count,
    B3TrafficReservationAgent* agents, int agent_count,
    int* owners, unsigned int owner_count) {
    if (!agents || agent_count <= 0) return;
    for (int agent_index = 0; agent_index < agent_count; agent_index++) {
        agents[agent_index].ahead = -1;
        agents[agent_index].behind = -1;
    }
    if (!paths || !owners) return;
    for (int owner_index = 0; owner_index < (int)owner_count; owner_index++)
        owners[owner_index] = -1;

    for (int agent_index = 0; agent_index < agent_count; agent_index++) {
        B3TrafficReservationAgent* agent = &agents[agent_index];
        if (!agent->active || agent->path_id >= path_count) continue;
        for (int candidate_index = 0; candidate_index < agent_count;
             candidate_index++) {
            B3TrafficReservationAgent* candidate = &agents[candidate_index];
            if (candidate_index == agent_index || !candidate->active
                || candidate->path_id != agent->path_id
                || !b3_traffic_agent_precedes(candidate, candidate_index,
                                              agent, agent_index))
                continue;
            if (agent->ahead < 0
                || b3_traffic_agent_precedes(&agents[agent->ahead],
                                              agent->ahead, candidate,
                                              candidate_index))
                agent->ahead = candidate_index;
        }
        if (agent->ahead >= 0)
            agents[agent->ahead].behind = agent_index;
    }

    /* PERF: this used to be O(paths * rows * agents) and ran EVERY frame --
     * 4,838 rows x 64 agents ~= 310k iterations per frame on US_C3_V1, which
     * measured at 17.8 fps against 124.9 fps with the pass skipped, i.e. the
     * whole "game feels slower" report.  The result is unchanged.
     *
     * b3_traffic_agent_precedes() replaces the incumbent whenever the
     * incumbent has the LARGER cursor (ties within epsilon going to the
     * larger index), so the owner of a row is exactly the eligible agent
     * minimising (cursor, index) -- the NEAREST agent at or ahead of it.
     * Eligibility (cursor + eps >= row) only ever shrinks as `row` rises, so
     * over a cursor-sorted list the answer is the first still-eligible entry
     * and the cursor into that list advances monotonically: O(n log n + rows)
     * per path instead of O(rows * n). */
    for (unsigned int path_index = 0; path_index < path_count; path_index++) {
        const B3TrafficReservationPath* path = &paths[path_index];
        const float epsilon = 1e-5f;
        int order[B3_TRAFFIC_RES_MAX_AGENTS];
        int n = 0;
        int cursor_at = 0;

        if (agent_count > B3_TRAFFIC_RES_MAX_AGENTS) {
            /* Fallback: more agents than the scratch array holds.  Identical
             * result, just the original exhaustive cost. */
            for (unsigned int row = 0; row < path->count; row++) {
                unsigned int owner_index = path->pair_base + row;
                int owner = -1;
                if (owner_index >= owner_count) continue;
                for (int agent_index = 0; agent_index < agent_count;
                     agent_index++) {
                    const B3TrafficReservationAgent* candidate =
                        &agents[agent_index];
                    if (!candidate->active || candidate->path_id != path_index
                        || candidate->cursor + epsilon < (float)row)
                        continue;
                    if (owner < 0 || b3_traffic_agent_precedes(
                            &agents[owner], owner, candidate, agent_index))
                        owner = agent_index;
                }
                owners[owner_index] = owner;
            }
            continue;
        }

        for (int agent_index = 0; agent_index < agent_count; agent_index++) {
            const B3TrafficReservationAgent* candidate = &agents[agent_index];
            if (!candidate->active || candidate->path_id != path_index)
                continue;
            order[n++] = agent_index;
        }
        /* insertion sort, ascending cursor then ascending index (n is the
         * per-path agent count -- a handful in practice) */
        for (int i = 1; i < n; i++) {
            int key = order[i];
            int j = i - 1;
            while (j >= 0
                   && (agents[order[j]].cursor > agents[key].cursor
                       || (agents[order[j]].cursor == agents[key].cursor
                           && order[j] > key))) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = key;
        }

        for (unsigned int row = 0; row < path->count; row++) {
            unsigned int owner_index = path->pair_base + row;
            int owner = -1;
            if (owner_index >= owner_count) continue;
            while (cursor_at < n
                   && agents[order[cursor_at]].cursor + epsilon < (float)row)
                cursor_at++;
            if (cursor_at < n) {
                owner = order[cursor_at];
                /* the comparator treats cursors within epsilon as equal and
                 * then prefers the smaller index -- honour that exactly */
                for (int q = cursor_at + 1; q < n; q++) {
                    if (agents[order[q]].cursor
                        > agents[order[cursor_at]].cursor + epsilon)
                        break;
                    if (order[q] < owner) owner = order[q];
                }
            }
            owners[owner_index] = owner;
        }
    }
}
