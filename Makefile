CC    = gcc
BUILD = build

# epoll 系列只在 Linux 上可编译
UNAME := $(shell uname)

COMMON_TARGETS = tcp_server select_server poll_server
LINUX_TARGETS  = epoll_server epoll_et reactor multi_reactor

ifeq ($(UNAME), Linux)
    TARGETS = $(COMMON_TARGETS) $(LINUX_TARGETS)
else
    TARGETS = $(COMMON_TARGETS)
    $(info [info] macOS detected: skipping epoll/reactor targets (Linux only))
endif

all: $(BUILD) $(addprefix $(BUILD)/, $(TARGETS))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tcp_server: tcp_server.c
	$(CC) -o $@ $^ -lpthread

$(BUILD)/select_server: select_server.c
	$(CC) -o $@ $^

$(BUILD)/poll_server: poll_server.c
	$(CC) -o $@ $^

$(BUILD)/epoll_server: epoll_server.c
	$(CC) -o $@ $^

$(BUILD)/epoll_et: epoll_et.c
	$(CC) -o $@ $^

$(BUILD)/reactor: reactor.c
	$(CC) -o $@ $^

$(BUILD)/multi_reactor: multi_reactor.c
	$(CC) -o $@ $^ -lpthread

clean:
	rm -rf $(BUILD)
