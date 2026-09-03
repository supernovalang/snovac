#!/bin/sh
# install_path.sh — persist BINDIR on PATH for bash, zsh and fish.
#
# Called by `make install` (see Makefile) once the snovac binary has been
# copied into BINDIR. Idempotent: re-running it after BINDIR is already on
# PATH in a given rc file is a no-op for that file.

set -e

BINDIR="$1"
if [ -z "$BINDIR" ]; then
    echo "install_path.sh: missing BINDIR argument" >&2
    exit 1
fi

add_line_if_missing() {
    file="$1"
    line="$2"

    if [ -f "$file" ] && grep -qF "$BINDIR" "$file" 2>/dev/null; then
        return 0
    fi

    mkdir -p "$(dirname "$file")"
    printf '\n# Added by snovac install (make install)\n%s\n' "$line" >> "$file"
    echo "  updated $file"
}

# bash: .bashrc is sourced for interactive non-login shells (Linux default);
# .bash_profile is sourced for login shells (macOS Terminal default). Cover
# both so PATH is picked up regardless of shell/session type.
for rc in "$HOME/.bashrc" "$HOME/.bash_profile"; do
    add_line_if_missing "$rc" "export PATH=\"$BINDIR:\$PATH\""
done

# zsh
add_line_if_missing "$HOME/.zshrc" "export PATH=\"$BINDIR:\$PATH\""

# fish uses a different syntax and config location entirely.
add_line_if_missing "$HOME/.config/fish/config.fish" "set -gx PATH $BINDIR \$PATH"

echo ""
echo "PATH updated for bash, zsh and fish."
echo "Restart your shell, or run the following in the current one:"
echo "  export PATH=\"$BINDIR:\$PATH\""
