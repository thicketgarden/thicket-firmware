#!/usr/bin/env bash
# Materialise the pinned microReticulum tree that the interop scenarios build
# against, at test_interop/.deps/microReticulum.
#
# Why a checkout of our own rather than letting PlatformIO fetch it per
# scenario project:
#
#   1. The scenarios need `examples/common/udp_interface`, which lives inside
#      the microReticulum repo. PlatformIO cannot depend on a repo
#      subdirectory, so the scenario projects symlink into a tree we control.
#   2. One tree shared by every scenario, rather than one clone per scenario.
#   3. The SHA is read out of the top-level platformio.ini, so the suite always
#      tests the stack we actually pin. It cannot drift from the firmware.
#
# The firmware's own libdeps checkout is used as the clone source when it is
# present (no network, and it is the same object store); otherwise we clone
# from GitHub.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS_DIR="$REPO_ROOT/test_interop/.deps"
TARGET="$DEPS_DIR/microReticulum"
PIO_INI="$REPO_ROOT/platformio.ini"

# The microReticulum line in [thicket] lib_deps, e.g.
#   https://github.com/wet-bulb/microReticulum.git#0fb6151f...
PIN_LINE="$(grep -Eo 'https://github.com/[^ ]*/microReticulum\.git#[0-9a-f]+' "$PIO_INI" | head -n1)"
if [[ -z "$PIN_LINE" ]]; then
  echo "[fetch_deps] could not find a pinned microReticulum URL in $PIO_INI" >&2
  exit 1
fi
PIN_URL="${PIN_LINE%%#*}"
PIN_SHA="${PIN_LINE##*#}"

echo "[fetch_deps] pinned microReticulum: $PIN_URL @ $PIN_SHA"

# The scenario projects that pull microLXMF / MsgPack / Crypto / microStore as
# git URLs have to name a SHA in their own platformio.ini -- PlatformIO has no
# way to inherit one. Duplicated pins rot silently, so check them here instead
# of hoping. A drifted pin means a scenario is testing a stack we do not ship,
# which is worse than no scenario at all.
drift=0
for lib in microLXMF MsgPack Crypto microStore; do
  # `|| true`: not every dependency is pinned by URL, and not every
  # scenario uses every dependency. A miss is normal; a mismatch is not.
  want="$(grep -Eo "https://github.com/[^ ]*/${lib}\.git#[0-9a-f]+" "$PIO_INI" | head -n1 || true)"
  [[ -z "$want" ]] && continue
  want_sha="${want##*#}"
  while IFS= read -r scenario_ini; do
    have="$(grep -Eo "https://github.com/[^ ]*/${lib}\.git#[0-9a-f]+" "$scenario_ini" | head -n1 || true)"
    [[ -z "$have" ]] && continue
    have_sha="${have##*#}"
    if [[ "$have_sha" != "$want_sha" ]]; then
      echo "[fetch_deps] PIN DRIFT: $lib" >&2
      echo "[fetch_deps]   platformio.ini: $want_sha" >&2
      echo "[fetch_deps]   $scenario_ini: $have_sha" >&2
      drift=1
    fi
  done < <(find "$REPO_ROOT/test_interop" -mindepth 2 -maxdepth 2 -name platformio.ini)
done
if [[ $drift -ne 0 ]]; then
  echo "[fetch_deps] a scenario pins a dependency the firmware does not. Fix" >&2
  echo "[fetch_deps] the scenario's platformio.ini before running the suite." >&2
  exit 1
fi

if [[ -d "$TARGET/.git" ]]; then
  HAVE="$(git -C "$TARGET" rev-parse HEAD 2>/dev/null || echo none)"
  if [[ "$HAVE" == "$PIN_SHA" ]]; then
    if [[ -z "$(git -C "$TARGET" status --porcelain)" ]]; then
      echo "[fetch_deps] $TARGET already at pin and clean"
      exit 0
    fi
    echo "[fetch_deps] $TARGET is at the pin but DIRTY; restoring" >&2
    git -C "$TARGET" checkout -- .
    git -C "$TARGET" clean -fd
    exit 0
  fi
  echo "[fetch_deps] $TARGET is at $HAVE, want $PIN_SHA; refetching"
  rm -rf "$TARGET"
fi

mkdir -p "$DEPS_DIR"

# Prefer the firmware's own libdeps checkout as a clone source: same objects,
# no network. Fall back to the remote.
SOURCE="$PIN_URL"
for env_dir in wiscore_rak4631 wiscore_rak4631-noble; do
  CAND="$REPO_ROOT/.pio/libdeps/$env_dir/microReticulum"
  if [[ -d "$CAND/.git" ]] && git -C "$CAND" cat-file -e "${PIN_SHA}^{commit}" 2>/dev/null; then
    SOURCE="$CAND"
    echo "[fetch_deps] cloning from local libdeps checkout $CAND"
    break
  fi
done

git clone --quiet "$SOURCE" "$TARGET"
git -C "$TARGET" checkout --quiet "$PIN_SHA"
echo "[fetch_deps] $TARGET now at $(git -C "$TARGET" rev-parse HEAD)"
