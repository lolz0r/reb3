#include "burnout3_collision.h"
#include "burnout3_vehicle_sim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_COLLISION_PATH "build/validate_frozen_soup_collision.bin"

typedef struct SoupRayContext {
    const B3CollisionPoly* polys;
    int count;
    int calls;
    int hits;
} SoupRayContext;

static int soup_ground_ray(void* user, const float start[3], const float end[3],
                           float* hit_t, float normal[3]) {
    SoupRayContext* context = user;
    context->calls++;
    int surface = b3_collision_ray_polys_game_space(context->polys,
                                                     context->count, start, end,
                                                     hit_t, normal);
    if (surface >= 0) context->hits++;
    return surface;
}

static int check_vehicle_uses_frozen_soup(const B3CollisionPoly* polys,
                                          int count) {
    B3PhysicsConfig config;
    B3VehicleFull vehicle;
    const float wheels_xz[4][2] = {
        {-1.0f, 1.5f}, {1.0f, 1.5f}, {-1.0f, -1.5f}, {1.0f, -1.5f}
    };
    const float half_ext[4] = {1.0f, 0.5f, 2.0f, 0.0f};
    const float center_off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float inv_inertia[3] = {1.0f, 1.0f, 1.0f};
    const float pos[3] = {0.0f, -0.25f, 0.0f};
    SoupRayContext context = {polys, count, 0, 0};

    b3_physics_defaults(&config);
    b3_vehicle_full_init(&vehicle, &config, wheels_xz, 0.35f, half_ext,
                         center_off, inv_inertia, pos, 0.0f);
    vehicle.soup_ground_user = &context;
    vehicle.soup_ground_ray = soup_ground_ray;
    b3_vehicle_step_full(&vehicle, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);

    if (context.calls != 10 || context.hits != 10
        || vehicle.wheel[0].contact == 0 || vehicle.wheel[1].contact == 0
        || vehicle.wheel[2].contact == 0 || vehicle.wheel[3].contact == 0
        || vehicle.ground_clear >= 30.0f) {
        fprintf(stderr, "vehicle did not use frozen soup rays (%d calls, %d hits)\n",
                context.calls, context.hits);
        return 0;
    }
    return 1;
}

static int check_loader_preserves_ground_winding(void) {
    unsigned char header[0x28] = {0};
    unsigned char record[40] = {0};
    const float game_vertices[3][3] = {
        {-100.0f, 0.0f, -100.0f}, {0.0f, 0.0f, 100.0f},
        {100.0f, 0.0f, -100.0f}
    };
    const float start[3] = {0.0f, 1.0f, 0.0f};
    const float end[3] = {0.0f, -2.0f, 0.0f};
    const float center[3] = {0.0f, 0.0f, 0.0f};
    const float half[3] = {20.0f, 2.0f, 20.0f};
    B3CollisionPoly polys[4];
    float hit_t, normal[3];
    FILE* file;

    memcpy(header, "B3CL", 4);
    {
        const unsigned version = 1;
        const unsigned count = 1;
        memcpy(header + 4, &version, sizeof(version));
        memcpy(header + 8, &count, sizeof(count));
    }
    memcpy(record, game_vertices, sizeof(game_vertices));
    {
        const unsigned short type = 0x11;
        memcpy(record + 36, &type, sizeof(type));
    }

    file = fopen(TEST_COLLISION_PATH, "wb");
    if (!file || fwrite(header, 1, sizeof(header), file) != sizeof(header)
        || fwrite(record, 1, sizeof(record), file) != sizeof(record)) {
        fprintf(stderr, "could not write collision winding fixture\n");
        if (file) fclose(file);
        remove(TEST_COLLISION_PATH);
        return 0;
    }
    fclose(file);
    if (!b3_collision_load(TEST_COLLISION_PATH)) {
        fprintf(stderr, "could not load collision winding fixture\n");
        remove(TEST_COLLISION_PATH);
        return 0;
    }
    remove(TEST_COLLISION_PATH);

    int count = b3_collision_gather(center, half, polys, 4);
    int surface = b3_collision_ray_polys_game_space(polys, count, start, end,
                                                     &hit_t, normal);
    if (count != 1 || surface != 0x11 || fabsf(hit_t - (1.0f / 3.0f)) > 1e-6f
        || fabsf(normal[0]) > 1e-6f || fabsf(normal[1] - 1.0f) > 1e-6f
        || fabsf(normal[2]) > 1e-6f) {
        fprintf(stderr, "collision loader lost one-sided ground winding\n");
        return 0;
    }
    return 1;
}

static int check_frozen_wall_filter(void) {
    B3CollisionPoly input[3];
    B3CollisionPoly output[3];
    memset(input, 0, sizeof(input));
    for (int index = 0; index < 3; index++) {
        input[index].v1[0] = 1.0f;
        input[index].v2[1] = 1.0f;
    }
    input[0].normal[0] = -1.0f;
    input[0].type = 0x11;
    input[1].normal[1] = 1.0f;
    input[1].type = 0x11;
    input[2].normal[0] = 1.0f;
    input[2].type = 0x15;
    const float center[3] = {0.0f, 0.0f, 0.0f};
    const float half[3] = {2.0f, 2.0f, 2.0f};
    const float velocity[3] = {1.0f, 0.0f, 0.0f};
    int count = b3_collision_filter_walls(input, 3, center, half, velocity,
                                          0.45f, output, 3);
    if (count != 1 || output[0].type != 0x11) {
        fprintf(stderr, "frozen soup wall filter regression\n");
        return 0;
    }
    return 1;
}

int main(void) {
    B3CollisionPoly polys[4];
    memset(polys, 0, sizeof(polys));
    float upper[3][3] = {{-100.0f, 0.0f, 100.0f}, {100.0f, 0.0f, 100.0f},
                          {-100.0f, 0.0f, -100.0f}};
    float upper_second[3][3] = {{100.0f, 0.0f, 100.0f},
                                 {100.0f, 0.0f, -100.0f},
                                 {-100.0f, 0.0f, -100.0f}};
    float lower[3][3] = {{-100.0f, -1.0f, 100.0f}, {100.0f, -1.0f, 100.0f},
                          {-100.0f, -1.0f, -100.0f}};
    float lower_second[3][3] = {{100.0f, -1.0f, 100.0f},
                                 {100.0f, -1.0f, -100.0f},
                                 {-100.0f, -1.0f, -100.0f}};
    memcpy(polys[0].v0, upper[0], sizeof(upper[0]));
    memcpy(polys[0].v1, upper[1], sizeof(upper[1]));
    memcpy(polys[0].v2, upper[2], sizeof(upper[2]));
    polys[0].normal[1] = 1.0f;
    polys[0].type = 0x11;
    memcpy(polys[1].v0, upper_second[0], sizeof(upper_second[0]));
    memcpy(polys[1].v1, upper_second[1], sizeof(upper_second[1]));
    memcpy(polys[1].v2, upper_second[2], sizeof(upper_second[2]));
    polys[1].normal[1] = 1.0f;
    polys[1].type = 0x11;
    memcpy(polys[2].v0, lower[0], sizeof(lower[0]));
    memcpy(polys[2].v1, lower[1], sizeof(lower[1]));
    memcpy(polys[2].v2, lower[2], sizeof(lower[2]));
    polys[2].normal[1] = 1.0f;
    polys[2].type = 0x12;
    memcpy(polys[3].v0, lower_second[0], sizeof(lower_second[0]));
    memcpy(polys[3].v1, lower_second[1], sizeof(lower_second[1]));
    memcpy(polys[3].v2, lower_second[2], sizeof(lower_second[2]));
    polys[3].normal[1] = 1.0f;
    polys[3].type = 0x12;
    if (!check_loader_preserves_ground_winding()) return 1;
    float start[3] = {0.0f, 1.0f, 0.0f};
    float end[3] = {0.0f, -2.0f, 0.0f};
    float hit_t, normal[3];
    int surface = b3_collision_ray_polys_game_space(polys, 4, start, end,
                                                     &hit_t, normal);
    if (surface != 0x11 || fabsf(hit_t - (1.0f / 3.0f)) > 1e-6f
        || fabsf(normal[0]) > 1e-6f || fabsf(normal[1] - 1.0f) > 1e-6f
        || fabsf(normal[2]) > 1e-6f) {
        fprintf(stderr, "frozen soup game-ray regression\n");
        return 1;
    }
    if (!check_vehicle_uses_frozen_soup(polys, 4)) return 1;
    if (!check_frozen_wall_filter()) return 1;
    puts("frozen soup ray, winding, vehicle prepass, and wall filter: OK");
    return 0;
}
