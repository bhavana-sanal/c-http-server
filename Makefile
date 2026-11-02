CC      ?= cc
CFLAGS  ?= -O2 -g
LDFLAGS ?=
BUILD   ?= build
FUZZ_SECONDS ?= 60

WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes
# _GNU_SOURCE exposes strptime/timegm/NI_MAXHOST under glibc; other libcs ignore it.
ALL_CFLAGS := -std=c17 $(WARNINGS) -D_GNU_SOURCE -Isrc $(CFLAGS)

ifeq ($(shell uname -s),Linux)
POLLER := poller_epoll
else
POLLER := poller_kqueue
endif

LIB_OBJS := $(addprefix $(BUILD)/,buf.o handlers.o http.o http_parser.o server.o static.o \
                                   timer_heap.o $(POLLER).o)
TESTS    := $(addprefix $(BUILD)/,test_parser test_static test_timer_heap)
BIN      := $(BUILD)/c-http-server
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

.PHONY: all test unit integration asan fuzz clean
.SECONDARY:

all: $(BIN)

$(BIN): $(BUILD)/main.o $(BUILD)/libserver.a
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/libserver.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: tests/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/test_%: $(BUILD)/test_%.o $(BUILD)/libserver.a
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/rawclient: $(BUILD)/rawclient.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD):
	mkdir -p $@

test: unit integration

unit: $(TESTS)
	@for t in $(TESTS); do $$t || exit 1; done

integration: $(BIN) $(BUILD)/rawclient
	tests/integration.sh $(BIN) $(BUILD)/rawclient

# Everything again under AddressSanitizer + UndefinedBehaviorSanitizer.
asan:
	$(MAKE) test BUILD=$(BUILD)/asan CFLAGS="-O1 -g $(SANITIZE)" LDFLAGS="$(SANITIZE)"

# Needs a clang that ships libFuzzer (stock LLVM on Linux; not Apple clang).
fuzz: | $(BUILD)
	clang $(ALL_CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined \
		-o $(BUILD)/fuzz_parser tests/fuzz_parser.c src/http_parser.c
	mkdir -p $(BUILD)/corpus
	$(BUILD)/fuzz_parser -max_total_time=$(FUZZ_SECONDS) -dict=tests/http.dict $(BUILD)/corpus

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
