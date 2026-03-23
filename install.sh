#!/usr/bin/env bash
# ─────────────────────────────────────────────
#  KITE language installer
#  Устанавливает в ~/.local/bin (без sudo)
# ─────────────────────────────────────────────
set -e

BINDIR="$HOME/.local/bin"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "==> Building KITE..."
cd "$SRC_DIR"
make clean 2>/dev/null || true
make

echo "==> Installing to $BINDIR/kite"
mkdir -p "$BINDIR"
cp kite "$BINDIR/kite"
chmod +x "$BINDIR/kite"

# Добавить в PATH если ещё нет
SHELL_RC=""
if [[ "$SHELL" == */zsh ]]; then
  SHELL_RC="$HOME/.zshrc"
elif [[ "$SHELL" == */bash ]]; then
  SHELL_RC="$HOME/.bashrc"
fi

if [[ -n "$SHELL_RC" ]]; then
  if ! grep -q 'HOME/.local/bin' "$SHELL_RC" 2>/dev/null; then
    echo '' >> "$SHELL_RC"
    echo '# KITE language' >> "$SHELL_RC"
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$SHELL_RC"
    echo "==> Added ~/.local/bin to PATH in $SHELL_RC"
  fi
fi

export PATH="$BINDIR:$PATH"

echo ""
echo "✓ Готово!"
echo "  Версия:  $(kite --version)"
echo ""
echo "  Перезапусти терминал или выполни:"
echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
echo ""
echo "  Затем:"
echo "    kite              # REPL"
echo "    kite file.kite    # запустить файл"
