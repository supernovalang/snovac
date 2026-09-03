# snovac — Snovalang compiler, C11, no cargo.
#
# Standalone on purpose: the repo-root Makefile still wraps `cargo xtask` for
# the Rust Stage 0, which snovac replaces only at phase P7 (see
# specs/20260719/snovac-c-toolchain/plan.md). Until then the two build systems
# coexist and neither depends on the other.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -pthread
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
WARN     = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wconversion -Wno-sign-conversion
BUILD   ?= build

# OS detection for `install`/`uninstall`: native Windows `make` (and
# MSYS2/Git Bash, which still inherit OS=Windows_NT from the environment)
# need a .exe suffix and a PowerShell-based PATH setup instead of the
# POSIX shell one used for Linux/macOS.
ifeq ($(OS),Windows_NT)
  EXE := .exe
  EXTRA_LIBS := -lws2_32
else
  EXE :=
  EXTRA_LIBS := -lpthread
endif

BIN      = $(BUILD)/sncli$(EXE)

SRCS = main.c driver_utils.c project.c cmd_check.c cmd_lex_parse.c cmd_run.c cmd_build.c cmd_tidy.c cmd_get.c \
       target.c native_backend.c pulsar.c async.c \
       dump.c ast.c \
       lex.c lex_token.c lex_literal.c \
       parse.c parse_type.c parse_expr.c parse_primary.c parse_stmt.c \
       parse_decl.c parse_decl_parts.c \
       eval.c eval_expr.c eval_stmt.c eval_string.c \
       socket_abi.c native_dispatch.c \
       diag.c arena.c intern.c symbol.c package.c types.c resolve.c builtins.c check.c \
       snbc.c value.c vm.c emit_bc.c link_append.c
OBJS = $(addprefix $(BUILD)/,$(SRCS:.c=.o))
DEPS = $(OBJS:.o=.d)

# intern.c/symbol.c/package.c/types.c have no CLI surface yet (P2.1/P2.2/P2.3)
# — exercised directly by standalone C test binaries instead of through
# $(BIN). See tests/test_symbol.c, tests/test_package.c, tests/test_types.c.
TEST_SYMBOL_BIN = $(BUILD)/test_symbol$(EXE)
TEST_PACKAGE_BIN = $(BUILD)/test_package$(EXE)
TEST_PACKAGE_OBJS = $(BUILD)/arena.o $(BUILD)/diag.o $(BUILD)/intern.o \
                     $(BUILD)/symbol.o $(BUILD)/package.o $(BUILD)/ast.o \
                     $(BUILD)/lex.o $(BUILD)/lex_token.o $(BUILD)/lex_literal.o
TEST_TYPES_BIN = $(BUILD)/test_types$(EXE)
TEST_TYPES_OBJS = $(BUILD)/arena.o $(BUILD)/intern.o $(BUILD)/symbol.o \
                   $(BUILD)/types.o
TEST_RESOLVE_BIN = $(BUILD)/test_resolve$(EXE)
TEST_RESOLVE_OBJS = $(BUILD)/arena.o $(BUILD)/diag.o $(BUILD)/intern.o \
                     $(BUILD)/symbol.o $(BUILD)/package.o $(BUILD)/types.o \
                     $(BUILD)/resolve.o $(BUILD)/ast.o \
                     $(BUILD)/lex.o $(BUILD)/lex_token.o $(BUILD)/lex_literal.o \
                     $(BUILD)/parse.o $(BUILD)/parse_type.o $(BUILD)/parse_expr.o \
                     $(BUILD)/parse_primary.o $(BUILD)/parse_stmt.o \
                     $(BUILD)/parse_decl.o $(BUILD)/parse_decl_parts.o
TEST_CHECK_BIN = $(BUILD)/test_check$(EXE)
TEST_CHECK_OBJS = $(TEST_RESOLVE_OBJS) $(BUILD)/builtins.o $(BUILD)/check.o

RT_SRCS = driver_utils.c project.c target.c native_backend.c pulsar.c async.c \
          dump.c ast.c \
          lex.c lex_token.c lex_literal.c \
          parse.c parse_type.c parse_expr.c parse_primary.c parse_stmt.c \
          parse_decl.c parse_decl_parts.c \
          eval.c eval_expr.c eval_stmt.c eval_string.c \
          socket_abi.c native_dispatch.c \
          diag.c arena.c intern.c symbol.c package.c types.c resolve.c builtins.c check.c \
          snbc.c value.c vm.c emit_bc.c link_append.c
RT_OBJS = $(addprefix $(BUILD)/,$(RT_SRCS:.c=.o))
LIB_RT  = $(BUILD)/libsnovart.a

# Default install prefix (~/.snova on Unix, %USERPROFILE%/.snova on Windows)
ifeq ($(OS),Windows_NT)
  PREFIX  ?= $(USERPROFILE)/.snova
else
  PREFIX  ?= $(HOME)/.snova
endif
BINDIR  ?= $(PREFIX)/bin
LIBDIR  ?= $(PREFIX)/lib
INCDIR  ?= $(PREFIX)/include

# snova-std lives as a sibling directory of snovac/ in the repo. It's
# optional: if it isn't checked out, install just skips it (SNOVA_STD_DIR
# or the project's own .snovalang/deps can supply it another way).
STD_SRC_DIR := ../snova-std/src
STD_INSTALL_DIR ?= $(HOME)/.snovalang/std/src

.PHONY: all clean test unit conformance install uninstall installer-windows

all: $(BIN) $(LIB_RT)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(EXTRA_LIBS)
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Copy-Item '$@' '$(BUILD)/snovac$(EXE)' -Force"
else
	@cp $@ $(BUILD)/snovac$(EXE) 2>/dev/null || true
endif

$(LIB_RT): $(RT_OBJS)
	ar rcs $@ $(RT_OBJS)

# Installs the snovac binary into BINDIR, its runtime static lib + headers
# (needed by `snovac build --runtime`, which shells out to $(CC) again at
# run time) into LIBDIR/INCDIR, snova-std (if present) into
# STD_INSTALL_DIR, and wires BINDIR onto PATH for every common shell:
#   - bash   (~/.bashrc and ~/.bash_profile)
#   - zsh    (~/.zshrc)
#   - fish   (~/.config/fish/config.fish)
#   - PowerShell (User PATH env var + $PROFILE), on Windows
# Each is idempotent, so re-running `make install` is safe.
install: $(BIN) $(LIB_RT)
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/install_windows.ps1 -Prefix "$(PREFIX)" -BinDir "$(BINDIR)" -LibDir "$(LIBDIR)" -IncDir "$(INCDIR)" -Bin "$(BIN)" -LibRt "$(LIB_RT)"
else
	@mkdir -p $(BINDIR) $(LIBDIR) $(INCDIR)
	install -m 755 $(BIN) $(BINDIR)/snovac$(EXE)
	@echo "✓ Installed snovac CLI to $(BINDIR)/snovac$(EXE)"
	install -m 644 $(LIB_RT) $(LIBDIR)/libsnovart.a
	install -m 644 *.h $(INCDIR)/
	@echo "✓ Installed runtime lib + headers to $(LIBDIR), $(INCDIR)"
	@if [ -d $(STD_SRC_DIR) ]; then \
		mkdir -p "$(STD_INSTALL_DIR)"; \
		cp -R $(STD_SRC_DIR)/. "$(STD_INSTALL_DIR)/"; \
		echo "✓ Installed snova-std to $(STD_INSTALL_DIR)"; \
	fi
	@sh scripts/install_path.sh "$(BINDIR)"
endif

uninstall:
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/uninstall_windows.ps1 -Prefix "$(PREFIX)" -BinDir "$(BINDIR)" -LibDir "$(LIBDIR)" -IncDir "$(INCDIR)"
else
	rm -f $(BINDIR)/snovac$(EXE)
	rm -f $(LIBDIR)/libsnovart.a
	rm -f $(addprefix $(INCDIR)/,$(notdir $(wildcard *.h)))
	@echo "✓ Removed snovac from $(BINDIR)/snovac$(EXE) (and its runtime lib/headers)"
	@echo "Note: PATH entries added by 'make install' in shell rc files /"
	@echo "the PowerShell profile are left untouched; remove them manually if desired."
	@echo "Note: snova-std installed to $(STD_INSTALL_DIR) is left untouched."
endif

installer-windows: $(BIN) $(LIB_RT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File installer/windows/build_installer.ps1

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -MMD -MP -c -o $@ $<

$(BUILD):
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "if (-not (Test-Path '$(BUILD)')) { New-Item -ItemType Directory -Path '$(BUILD)' | Out-Null }"
else
	mkdir -p $(BUILD)
endif

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
ifeq ($(OS),Windows_NT)
	@$(TEST_SYMBOL_BIN)
	@$(TEST_PACKAGE_BIN)
	@$(TEST_TYPES_BIN)
	@$(TEST_RESOLVE_BIN)
	@$(TEST_CHECK_BIN)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "if (Get-Command sh -ErrorAction SilentlyContinue) { sh tests/run.sh $(BIN) } elseif (Test-Path 'C:\Program Files\Git\bin\sh.exe') { & 'C:\Program Files\Git\bin\sh.exe' tests/run.sh $(BIN) } else { Write-Host 'Note: tests/run.sh skipped (requires bash/sh shell)' }"
else
	@sh tests/run.sh $(BIN)
	@./$(TEST_SYMBOL_BIN)
	@./$(TEST_PACKAGE_BIN)
	@./$(TEST_TYPES_BIN)
	@./$(TEST_RESOLVE_BIN)
	@./$(TEST_CHECK_BIN)
endif

# Lexes every .snova in the repository and reports coverage.
conformance: $(BIN)
	@sh scripts/snovac-conformance.sh $(BIN)

test: unit

clean:
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "if (Test-Path '$(BUILD)') { Remove-Item -Path '$(BUILD)' -Recurse -Force }"
else
	rm -rf $(BUILD)
endif

-include $(DEPS)
