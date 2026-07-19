# snovac — Snovalang compiler, C11, no cargo.
#
# Standalone on purpose: the repo-root Makefile still wraps `cargo xtask` for
# the Rust Stage 0, which snovac replaces only at phase P7 (see
# specs/20260719/snovac-c-toolchain/plan.md). Until then the two build systems
# coexist and neither depends on the other.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
WARN     = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wconversion -Wno-sign-conversion
BUILD   ?= build
BIN      = $(BUILD)/snovac

SRCS = main.c lex.c parse.c diag.c arena.c
OBJS = $(addprefix $(BUILD)/,$(SRCS:.c=.o))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean test unit conformance

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# Assertions for the lexer decisions derived from the corpus.
unit: $(BIN)
	@sh tests/run.sh $(BIN)

# Lexes every .snova in the repository and reports coverage.
conformance: $(BIN)
	@sh ../scripts/snovac-conformance.sh $(BIN)

test: unit conformance

clean:
	rm -rf $(BUILD)

-include $(DEPS)
