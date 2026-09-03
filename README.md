# snovac — Snovalang Compiler

The official, reference compiler for **Snovalang** written in pure, zero-dependency **ISO C11**.

[![Release](https://github.com/supernovalang/snovac/actions/workflows/release.yml/badge.svg)](https://github.com/supernovalang/snovac/actions/workflows/release.yml)

## Quick Install (One-Line URL | Bash)

Install `snovac` instantly on macOS or Linux with a single command:

```bash
curl -fsSL https://raw.githubusercontent.com/supernovalang/snovac/master/install.sh | bash
```

Or using `wget`:
```bash
wget -qO- https://raw.githubusercontent.com/supernovalang/snovac/master/install.sh | bash
```

## Features

- **Pure C11 Implementation**: Zero dependencies, extremely fast compilation speed.
- **Diagnostics & Error Reporting**: Colorized source snippets with precise diagnostic codes (`SNOVA0001` - `SNOVA0040`).
- **Module Management**: Discovers root packages with `mod.sno`.
- **Bytecode VM & Native Compilation**: Ahead-of-time bytecode compilation and interpretation.
- **Pulsar Concurrency**: Actor model and streaming concurrency support.

## Building from Source

### Prerequisites
- C11-compliant C compiler (`clang` or `gcc`)
- GNU `make`

### Build Command
```bash
git clone https://github.com/supernovalang/snovac.git
cd snovac
make
```

The compiled binary will be located at `build/snovac`.

## Usage & CLI Reference

### Informational & Diagnostics

- **`snovac --version`** (`-V`):
  Displays the current version of the compiler.

- **`snovac --help`** (`-h`):
  Displays command-line usage instructions and available options.

- **`snovac --target-info`**:
  Prints host and target architectures, detected operating system, executable paths, and active environment overrides (`SNOVA_TARGET_OS`, `SNOVA_TARGET_ARCH`, `SNOVA_TARGET`).

### Single-File Inspection & Compilation

- **`snovac --emit=tokens <file.snova>`**:
  Runs lexical analysis and dumps the token stream with source spans (line:col).

- **`snovac --check-lex <file.snova>`**:
  Validates lexical tokens without generating an AST; exits with non-zero code on syntax/lexer errors.

- **`snovac --emit=ast <file.snova>`**:
  Parses the source file and dumps the formatted AST (Abstract Syntax Tree).

- **`snovac --check-parse <file.snova>`**:
  Performs lexical analysis and syntax parsing; reports syntax errors with diagnostics.

- **`snovac check <file.snova>`**:
  Performs symbol resolution, scope analysis, and static type-checking on a single file.

- **`snovac run <file.snova>`**:
  Compiles and directly executes a single Snovalang source file in the bytecode VM runtime.

- **`snovac build <file.snova> [-o output] [--target=triple]`**:
  Compiles a single file to a standalone native binary or bytecode unit. Supports cross-compilation target triples (e.g. `aarch64-apple-darwin`, `x86_64-linux-gnu`).

### Package & Dependency Management

- **`snovac get [<repo-url>] [--version=<ver>] [--project=<path>]`**:
  Fetches dependencies into `.snovalang/deps/` and manages the `mod.sno` manifest.
  - **Adding a direct dependency**:
    ```bash
    # Add dependency with automatic or default version
    snovac get https://github.com/supernovalang/snova-http

    # Add dependency with a specific version or tag
    snovac get https://github.com/supernovalang/snova-http --version 1.0.0
    snovac get github.com/supernovalang/snova-http@1.0.0
    ```
  - **Sychronizing existing dependencies**:
    ```bash
    # Resolves and downloads all dependencies declared in mod.sno
    snovac get
    snovac get --project ./my-project
    ```
  - **Features**:
    - **Transitive Resolution**: Recursively fetches dependencies declared in dependencies' manifests.
    - **Deduplication & Diamond Graphs**: Shared dependencies ($A \to C$, $B \to C$) are cloned once and reused across all modules.
    - **Cycle Detection**: Identifies circular dependency loops ($A \to B \to A$) and reports the diagnostic path.
    - **Direct vs Indirect Classification**: Classifies root dependencies under `direct = [...]` and transitive edges under `indirect = ["from -> to"]` in `mod.sno`.
    - **Safe Execution**: Uses direct process invocation without shell string interpolation.
    - **Idempotency**: Running `get` multiple times preserves modifications, does not re-clone existing folders, and outputs deterministic manifests.

- **`snovac tidy [--project] [<path>]`**:
  Scans project imports across all source files, removes unused dependencies, and synchronizes `mod.sno`.
  ```bash
  snovac tidy
  snovac tidy --project ./my-project
  ```

### Project-Wide Operations

A project is discovered by locating the nearest `mod.sno`, `snova.sno`, or `snova.toml` manifest. Source roots include `src/` (or project root) and vendored `.snovalang/deps/`.

- **`snovac --check-parse-project <path>`**:
  Recursively discovers and parses all `.snova` source files across the project and dependencies.

- **`snovac check --project <path> [--no-typecheck]`**:
  Builds the project package graph, links imports, resolves types and symbols across packages, and type-checks declaration bodies. Adding `--no-typecheck` skips body checks while verifying interface signatures and imports.

- **`snovac run --project <path> [--offline-cache[=<dir>]]`**:
  Executes a multi-file project across its packages and resolved dependencies.

- **`snovac build --project <path> [-o output] [--target=triple] [--runtime] [--offline-cache[=<dir>]]`**:
  Compiles an entire project and its dependencies into a bundled executable. `--runtime` links the native runtime library into the final binary.

## Environment Variables

- `SNOVA_TARGET_OS`: Override target OS (`darwin`, `linux`, `windows`, `freebsd`).
- `SNOVA_TARGET_ARCH`: Override target architecture (`arm64`, `aarch64`, `x86_64`, `arm`).
- `SNOVA_TARGET`: Override target triple (e.g. `x86_64-unknown-linux-gnu`).
- `SNOVA_STD_DIR`: Explicit path to standard library sources (`snova-std/src`).
- `SNOVA_BUILTIN_DIR`: Explicit path to built-in definition files (`builtin/`).

## Documentation Standard

Snovalang uses structured doc comments formatted as:
```snova
/* -- Doc:{funcName}
 *
 * -- Description: Function description text.
 *
 * -- Param{paramName}: Parameter details.
 * -- Returns: Return value information.
 */
```

## License
MIT License
