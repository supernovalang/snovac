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
- **Module Management**: Discovers root packages with `snova.mdlo`.
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

## Usage

```bash
# Check syntax and types of a Snovalang file
snovac --check main.snova

# Parse check only
snovac --check-parse main.snova

# Run source file directly
snovac run main.snova

# Build standalone executable / bytecode
snovac build -o app main.snova
```

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
