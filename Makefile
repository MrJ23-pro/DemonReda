CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -std=gnu17 -pedantic -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS ?=

BUILD_DIR := build
SHARED_SRCS := src/shared/utils.c src/shared/proto.c src/shared/scheduler.c src/shared/storage.c
ERRAID_SRCS := src/erraid/main.c src/erraid/daemon.c src/erraid/executor.c src/erraid/notifier.c
TADMOR_SRCS := src/tadmor/main.c src/tadmor/request.c

SHARED_OBJS := $(SHARED_SRCS:src/shared/%.c=$(BUILD_DIR)/shared/%.o)
ERRAID_OBJS := $(ERRAID_SRCS:src/erraid/%.c=$(BUILD_DIR)/erraid/%.o)
TADMOR_OBJS := $(TADMOR_SRCS:src/tadmor/%.c=$(BUILD_DIR)/tadmor/%.o)

BIN_ER := erraid
BIN_TA := tadmor

.PHONY: all clean distclean format

all: $(BIN_ER) $(BIN_TA)

$(BIN_ER): $(SHARED_OBJS) $(ERRAID_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BIN_TA): $(SHARED_OBJS) $(TADMOR_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/shared/%.o: src/shared/%.c | $(BUILD_DIR)/shared
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/erraid/%.o: src/erraid/%.c | $(BUILD_DIR)/erraid
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tadmor/%.o: src/tadmor/%.c | $(BUILD_DIR)/tadmor
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/shared:
	@mkdir -p $@

$(BUILD_DIR)/erraid:
	@mkdir -p $@

$(BUILD_DIR)/tadmor:
	@mkdir -p $@

clean:
	rm -f $(SHARED_OBJS) $(ERRAID_OBJS) $(TADMOR_OBJS)

reset-pipes:
	rm -f /tmp/$(USER)/erraid/pipes/erraid-request-pipe /tmp/$(USER)/erraid/pipes/erraid-reply-pipe

RUNDIR ?= /tmp/$(USER)/erraid

clean-run:
	rm -rf $(RUNDIR)/tasks $(RUNDIR)/logs $(RUNDIR)/pipes $(RUNDIR)/state

format:
	clang-format -i $(SHARED_SRCS) $(ERRAID_SRCS) $(TADMOR_SRCS) include/*.h || true

distclean: clean clean-run
	rm -f $(BIN_ER) $(BIN_TA)
	rm -rf $(BUILD_DIR)
