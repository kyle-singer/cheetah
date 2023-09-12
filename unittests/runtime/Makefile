# TODO: Use cmake

RT_ROOT=../..
RT_DIR=$(RT_ROOT)/runtime
DBG_SRC=$(RT_DIR)/debug.c

TESTS = local-hypertable

CFLAGS += -I$(RT_ROOT)
LDLIBS += -lcriterion

local-hypertable: local-hypertable.c $(RT_DIR)/local-hypertable.c $(DBG_SRC)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(TESTS) *~ *.o
