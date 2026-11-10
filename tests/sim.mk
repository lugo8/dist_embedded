# Host-side tests: the Raft core in a 3-node, 2-bus simulation, once per leader mode.
#   make -C tests -f sim.mk     build and run everything
ROOT   := ..
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror
SRC    := raft_sim.c $(ROOT)/common/raft.c

all: run

build/cockpit/raft_conf.h: $(ROOT)/raft.conf $(ROOT)/tools/gen_raft_conf.py
	@mkdir -p $(@D)
	python3 $(ROOT)/tools/gen_raft_conf.py $< $@

build/random/raft.conf: $(ROOT)/raft.conf
	@mkdir -p $(@D)
	sed -e 's/^default_leader *=.*/default_leader = random/' $(if $(SEED),-e 's/^seed *=.*/seed = $(SEED)/') $< > $@
build/random/raft_conf.h: build/random/raft.conf
	python3 $(ROOT)/tools/gen_raft_conf.py $< $@

SEED ?=
build/sim_cockpit: build/cockpit/raft_conf.h $(SRC)
	$(CC) $(CFLAGS) -Ibuild/cockpit -I$(ROOT)/common $(SRC) -o $@
build/sim_random: build/random/raft_conf.h $(SRC)
	$(CC) $(CFLAGS) -Ibuild/random -I$(ROOT)/common $(SRC) -o $@

build/ts_sim: ts_sim.c $(ROOT)/common/timesync.c $(ROOT)/common/timesync.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(ROOT)/common ts_sim.c $(ROOT)/common/timesync.c -o $@

run: build/sim_cockpit build/sim_random build/ts_sim
	./build/sim_cockpit
	./build/sim_random
	./build/ts_sim

clean:
	rm -rf build
.PHONY: all run clean
