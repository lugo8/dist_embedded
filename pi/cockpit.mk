# Pi cockpit (N1).   make -C pi -f cockpit.mk     then   sudo ./pi/cockpit
# raft.conf (shared with the STM32 builds) -> build/raft_conf.h
ROOT    := ..
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Ibuild -I$(ROOT)/common
LDLIBS  := -lpthread -llgpio
SRC     := cockpit.c raft_node.c $(ROOT)/common/raft.c $(ROOT)/common/timesync.c
HDR     := $(wildcard *.h $(ROOT)/common/*.h) build/raft_conf.h

cockpit: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)

build/raft_conf.h: $(ROOT)/raft.conf $(ROOT)/tools/gen_raft_conf.py
	@mkdir -p build
	python3 $(ROOT)/tools/gen_raft_conf.py $< $@

clean:
	rm -rf build cockpit
.PHONY: clean
