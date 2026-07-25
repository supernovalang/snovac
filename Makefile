# snovac — Snovalang compiler, C11, no cargo.
#
# Standalone on purpose: the repo-root Makefile still wraps `cargo xtask` for
# the Rust Stage 0, which snovac replaces only at phase P7 (see
# specs/20260719/snovac-c-toolchain/plan.md). Until then the two build systems
# coexist and neither depends on the other.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
CPPFLAGS ?=
WARN     = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wconversion -Wno-sign-conversion
BUILD   ?= build
BIN      = $(BUILD)/snovac

SRCS = main.c dump.c ast.c \
       lex.c lex_token.c lex_literal.c \
       parse.c parse_type.c parse_expr.c parse_primary.c parse_stmt.c \
       parse_decl.c parse_decl_parts.c \
       eval.c eval_expr.c eval_stmt.c eval_string.c \
       diag.c arena.c intern.c symbol.c package.c types.c resolve.c builtins.c check.c
OBJS = $(addprefix $(BUILD)/,$(SRCS:.c=.o))
DEPS = $(OBJS:.o=.d)

# intern.c/symbol.c/package.c/types.c have no CLI surface yet (P2.1/P2.2/P2.3)
# — exercised directly by standalone C test binaries instead of through
# $(BIN). See tests/test_symbol.c, tests/test_package.c, tests/test_types.c.
TEST_SYMBOL_BIN = $(BUILD)/test_symbol
TEST_PACKAGE_BIN = $(BUILD)/test_package
TEST_PACKAGE_OBJS = $(BUILD)/arena.o $(BUILD)/diag.o $(BUILD)/intern.o \
                     $(BUILD)/symbol.o $(BUILD)/package.o $(BUILD)/ast.o \
                     $(BUILD)/lex.o $(BUILD)/lex_token.o $(BUILD)/lex_literal.o
TEST_TYPES_BIN = $(BUILD)/test_types
TEST_TYPES_OBJS = $(BUILD)/arena.o $(BUILD)/intern.o $(BUILD)/symbol.o \
                   $(BUILD)/types.o
TEST_RESOLVE_BIN = $(BUILD)/test_resolve
TEST_RESOLVE_OBJS = $(BUILD)/arena.o $(BUILD)/diag.o $(BUILD)/intern.o \
                     $(BUILD)/symbol.o $(BUILD)/package.o $(BUILD)/types.o \
                     $(BUILD)/resolve.o $(BUILD)/ast.o \
                     $(BUILD)/lex.o $(BUILD)/lex_token.o $(BUILD)/lex_literal.o \
                     $(BUILD)/parse.o $(BUILD)/parse_type.o $(BUILD)/parse_expr.o \
                     $(BUILD)/parse_primary.o $(BUILD)/parse_stmt.o \
                     $(BUILD)/parse_decl.o $(BUILD)/parse_decl_parts.o
TEST_CHECK_BIN = $(BUILD)/test_check
TEST_CHECK_OBJS = $(TEST_RESOLVE_OBJS) $(BUILD)/builtins.o $(BUILD)/check.o

.PHONY: all clean test unit conformance

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

$(TEST_SYMBOL_BIN): tests/test_symbol.c $(BUILD)/arena.o $(BUILD)/intern.o $(BUILD)/symbol.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -o $@ tests/test_symbol.c \
	    $(BUILD)/arena.o $(BUILD)/intern.o $(BUILD)/symbol.o

$(TEST_PACKAGE_BIN): tests/test_package.c $(TEST_PACKAGE_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -o $@ tests/test_package.c $(TEST_PACKAGE_OBJS)

$(TEST_TYPES_BIN): tests/test_types.c $(TEST_TYPES_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -o $@ tests/test_types.c $(TEST_TYPES_OBJS)

$(TEST_RESOLVE_BIN): tests/test_resolve.c $(TEST_RESOLVE_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -o $@ tests/test_resolve.c $(TEST_RESOLVE_OBJS)

$(TEST_CHECK_BIN): tests/test_check.c $(TEST_CHECK_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -o $@ tests/test_check.c $(TEST_CHECK_OBJS)

# Assertions for the lexer decisions derived from the corpus, plus the
# standalone symbol-table, package-graph, type-representation, resolver and
# checker unit tests.
unit: $(BIN) $(TEST_SYMBOL_BIN) $(TEST_PACKAGE_BIN) $(TEST_TYPES_BIN) $(TEST_RESOLVE_BIN) $(TEST_CHECK_BIN)
	@sh tests/run.sh $(BIN)
	@./$(TEST_SYMBOL_BIN)
	@./$(TEST_PACKAGE_BIN)
	@./$(TEST_TYPES_BIN)
	@./$(TEST_RESOLVE_BIN)
	@./$(TEST_CHECK_BIN)

# Lexes every .snova in the repository and reports coverage.
conformance: $(BIN)
	@sh ../scripts/snovac-conformance.sh $(BIN)

test: unit conformance

clean:
	rm -rf $(BUILD)

-include $(DEPS)
