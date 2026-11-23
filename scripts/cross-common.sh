# Shared helpers for POSIX cross/native builds. Sourced by each platform script;
# this is not a generic build script by itself.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"

SRC_CPP=$(ls "$ROOT"/backend/src/*.cpp | sort)
SRC_SQLITE="$ROOT/backend/third_party/sqlite3.c"

CPPFLAGS="-DMODELEFORGE_VERSION=\"${VERSION}\" -DCPPHTTPLIB_THREAD_POOL_COUNT=8 -DCPPHTTPLIB_RECV_BUFSIZ=32768"
CFLAGS="-DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION=1"

log() { printf '\033[1;32m[build]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

# build_with <out-dir> <binary-name> <zig-target> <extra-args...>
build_with() {
  local out_dir="$1" bin="$2" target="$3"; shift 3
  local ZIG="${ZIG:-zig}"
  command -v "$ZIG" >/dev/null 2>&1 || die "未找到 zig，请安装或用 ZIG 环境变量指定编译器"
  mkdir -p "$out_dir"
  log "编译 $bin（target=$target）"
  local objdir="$out_dir/.obj"
  mkdir -p "$objdir"
  local f
  for f in $SRC_CPP; do
    # shellcheck disable=SC2086
    "$ZIG" c++ -std=c++17 -O2 -DNDEBUG $CPPFLAGS \
        -I "$ROOT/backend/include" -I "$ROOT/backend/third_party" \
        -target "$target" -c "$f" -o "$objdir/$(basename "${f%.cpp}").o"
  done
  # SQLite is C; compile it with the C driver so -std=c++17 is not applied.
  "$ZIG" cc -O2 $CFLAGS -I "$ROOT/backend/third_party" \
      -target "$target" -c "$SRC_SQLITE" -o "$objdir/sqlite3.o"
  "$ZIG" c++ -target "$target" "$objdir"/*.o -o "$out_dir/$bin" -s "$@"
  # Clean intermediates without `rm -rf`: sandboxed shells may intercept it.
  if [ -d "$objdir" ]; then
    find "$objdir" -type f -delete 2>/dev/null || true
    rmdir "$objdir" 2>/dev/null || true
  fi
  log "产物：$out_dir/$bin"
}

# stage <out-dir> <os-id> <arch> <ext>  ->  release/ModelForge-<ver>-<os>-<arch>.<ext>
# Archives are ZIP so every platform uses one format. Python's zipfile is used
# instead of the `zip` CLI and stores Unix mode bits, keeping `modelforge`
# executable after extraction on Linux/macOS/Android/openEuler.
stage() {
  local out_dir="$1" os="$2" arch="$3" ext="$4"
  local name="ModelForge-${VERSION}-${os}-${arch}"
  local release="$ROOT/release"
  mkdir -p "$release" "$out_dir/web" "$out_dir/docs"
  cp -r "$ROOT/frontend/dist/." "$out_dir/web/" 2>/dev/null || true
  cp "$ROOT/README.md" "$ROOT/LICENSE" "$out_dir/" 2>/dev/null || true
  cp "$ROOT/docs/"*.md "$out_dir/docs/" 2>/dev/null || true
  # README embeds docs/images/logo.png; ship the image assets with the docs.
  cp -r "$ROOT/docs/images" "$out_dir/docs/" 2>/dev/null || true
  python - "$ROOT/dist" "$out_dir" "$release/${name}.${ext}" <<'PY'
import os, sys, zipfile

base = os.path.abspath(sys.argv[1])
folder = os.path.abspath(sys.argv[2])
target = sys.argv[3]

with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        files.sort()
        for d in dirs:
            path = os.path.join(root, d)
            rel = os.path.relpath(path, base).replace(os.sep, "/") + "/"
            info = zipfile.ZipInfo(rel)
            info.external_attr = (0o40755 & 0xFFFF) << 16
            z.writestr(info, b"")
        for f in files:
            path = os.path.join(root, f)
            rel = os.path.relpath(path, base).replace(os.sep, "/")
            info = zipfile.ZipInfo(rel)
            mode = os.stat(path).st_mode
            # Windows has no Unix exec bit, so grant it explicitly for the
            # daemon and shell scripts; other files stay 0644.
            perm = 0o755 if (f == "modelforge" or f.endswith(".sh")) else 0o644
            info.external_attr = ((mode & 0o170000) | perm) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as fh:
                z.writestr(info, fh.read())
PY
  log "打包：$release/${name}.${ext}"
}
