# Host unit tests: compile the protocol/parser sources with the host compiler and run them.
CC      ?= cc
CFLAGS  ?= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -DAITERM_HOST_TEST
MAIN    := ../main
INC     := -I$(MAIN) -I$(MAIN)/util -I. -I../components/cjson
BUILD   := build

UTIL_SRCS := $(MAIN)/util/jsonbuf.c $(MAIN)/util/sse.c
HIST      := $(MAIN)/agent/history.c
CJSON     := ../components/cjson/cJSON.c

TESTS := test_sse test_openai test_anthropic test_mcp test_markdown test_diff test_vt test_history test_chron

.PHONY: all run clean
all: $(addprefix $(BUILD)/,$(TESTS))

run: all
	@fail=0; total=0; for t in $(TESTS); do \
	  if ./$(BUILD)/$$t; then :; else fail=$$((fail+1)); fi; total=$$((total+1)); \
	done; echo "host tests: $$total binaries, $$fail failed"; test $$fail -eq 0

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_sse: test_sse.c $(UTIL_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_openai: test_openai.c $(UTIL_SRCS) $(HIST) $(MAIN)/net/openai.c $(CJSON) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_anthropic: test_anthropic.c $(UTIL_SRCS) $(HIST) $(MAIN)/net/anthropic.c $(MAIN)/net/openai.c $(CJSON) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_mcp: test_mcp.c $(UTIL_SRCS) $(MAIN)/net/mcp_proto.c $(CJSON) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_markdown: test_markdown.c $(UTIL_SRCS) $(MAIN)/util/markdown.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_diff: test_diff.c $(UTIL_SRCS) $(MAIN)/util/linediff.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_vt: test_vt.c $(UTIL_SRCS) $(MAIN)/ui/vt_screen.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_history: test_history.c $(UTIL_SRCS) $(HIST) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_chron: test_chron.c $(UTIL_SRCS) $(MAIN)/net/chron_proto.c $(MAIN)/net/nkeys.c $(CJSON) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

clean:
	rm -rf $(BUILD)
