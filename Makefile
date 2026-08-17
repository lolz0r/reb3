# Burnout 3: Takedown - Makefile
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2
LDFLAGS = $(shell pkg-config --cflags --libs sdl2 SDL2_image) -lGL -lm

.PHONY: all clean clean-assets run assets check-assets elf \
	test-nav-walk test-nav-selector test-soup-ray test-traffic-paths \
	test-traffic-reservations test-traffic-pool test-traffic-mix

all: burnout3 build/dump_traj

# ---------------------------------------------------------------------------
# Retail-derived data.
#
# None of this is in the repository -- it is extracted from YOUR copy of the
# game. Point B3_GAME_ROOT at the folder holding default.xbe, then `make
# assets`. Full walkthrough: docs/ASSETS.md
# ---------------------------------------------------------------------------
GENERATED = src/burnout3_physics_params.h \
            src/burnout3_vehicle_data.h \
            src/burnout3_car_physics.h \
            src/burnout3_font.h \
            src/burnout3_track_paths.h \
            src/burnout3_traffic_data.h \
            src/burnout3_start_grid.h

MISSING = $(strip $(foreach h,$(GENERATED),$(if $(wildcard $(h)),,$(h))))

check-assets:
ifneq ($(MISSING),)
	@echo   ""
	@echo   "  This tree has no retail data in it, by design."
	@echo   ""
	@echo   "  Missing generated header(s):"
	@$(foreach h,$(MISSING),echo "      $(h)";)
	@echo   ""
	@echo   "  Set B3_GAME_ROOT to your own copy of the game and extract:"
	@echo   ""
	@echo   "      export B3_GAME_ROOT=\"/path/to/Burnout 3 Takedown\""
	@echo   "      make assets"
	@echo   ""
	@echo   "  See docs/ASSETS.md for the full walkthrough."
	@echo   ""
	@exit 1
endif

# The corrected ELF every RE tool reads. Derived from your default.xbe.
build/burnout3.elf:
	@mkdir -p build
	python3 tools/xbe2elf.py "$$B3_GAME_ROOT/default.xbe" build/burnout3.elf

elf: build/burnout3.elf

# The compiled-in data tables. extract_physics_params.py additionally needs a
# running Ghidra bridge -- see docs/ASSETS.md, "The one manual step".
assets: build/burnout3.elf
	python3 tools/extract_vehicles.py
	python3 tools/extract_physics_params.py
	python3 tools/extract_car_vdb.py generate
	python3 tools/extract_font.py
	python3 tools/extract_bgd_paths.py
	python3 tools/extract_traffic.py
	python3 tools/extract_start_grid.py
	@echo "generated headers written into src/ -- now run: make"

test-soup-ray: build/validate_frozen_soup
	./build/validate_frozen_soup

test-nav-walk:
	python3 tools/validate_nav_walk.py

test-nav-selector:
	python3 tools/validate_nav_selector.py

test-traffic-paths:
	python3 tools/validate_traffic_paths.py

test-traffic-mix:
	python3 tools/validate_traffic_mix.py

test-traffic-reservations: build/validate_traffic_reservations
	./build/validate_traffic_reservations

test-traffic-pool: build/validate_traffic_pool
	./build/validate_traffic_pool

build/validate_frozen_soup: tools/validate_frozen_soup.c src/burnout3_collision.c src/burnout3_collision.h src/burnout3_vehicle_sim.c src/burnout3_vehicle_sim.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tools/validate_frozen_soup.c src/burnout3_collision.c src/burnout3_vehicle_sim.c -lm

build/validate_traffic_reservations: tools/validate_traffic_reservations.c src/burnout3_traffic_reservations.c src/burnout3_traffic_reservations.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tools/validate_traffic_reservations.c src/burnout3_traffic_reservations.c -lm

build/validate_traffic_pool: tools/validate_traffic_pool.c src/burnout3_traffic_pool.c src/burnout3_traffic_pool.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tools/validate_traffic_pool.c src/burnout3_traffic_pool.c -lm

# trajectory driver for the full-pipeline differential test
# (tools/validate_port.py, full-pipeline section)
build/dump_traj: tools/dump_traj.c src/burnout3_vehicle_sim.c src/burnout3_panels.c src/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tools/dump_traj.c \
	    src/burnout3_vehicle_sim.c src/burnout3_panels.c -lm

SRCS = src/burnout3_full.c src/burnout3_vehicle_sim.c src/burnout3_trackmesh.c \
       src/burnout3_hud.c src/burnout3_collision.c src/burnout3_crash.c \
       src/burnout3_carcol.c src/burnout3_takedown.c src/burnout3_score_events.c \
       src/burnout3_td_rules.c src/burnout3_sfx.c src/burnout3_ai.c \
       src/burnout3_carfx.c src/burnout3_postfx.c src/burnout3_music.c \
       src/burnout3_boostfx.c src/burnout3_particlefx.c \
       src/burnout3_panels.c src/burnout3_props.c \
	       src/burnout3_traffic_reservations.c src/burnout3_traffic_pool.c

burnout3: check-assets $(SRCS) src/*.h
	$(CC) $(CFLAGS) -Isrc $(SRCS) $(LDFLAGS) -o $@

run: burnout3
	./burnout3

clean:
	rm -f burnout3 burnout3_full build/dump_traj build/validate_frozen_soup \
		build/validate_traffic_reservations build/validate_traffic_pool

# Remove the extracted data tables too. Does NOT touch build/, which holds
# multiple GB that take a long time to re-extract.
clean-assets:
	rm -f $(GENERATED)
