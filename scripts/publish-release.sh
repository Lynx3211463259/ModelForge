#!/usr/bin/env bash
# Publish ModelForge to GitHub in one pass:
#   1. create the repository when it does not exist
#   2. push the main branch and the version tag
#   3. create the Release from docs/release-notes-<version>.md
#   4. upload every packaged asset plus SHA256SUMS.txt
#
# Usage:
#   GITHUB_TOKEN=github_pat_xxx bash scripts/publish-release.sh
#
# Optional overrides:
#   GITHUB_OWNER  default Lynx3211463259
#   GITHUB_REPO   default ModelForge
#   VERSION       default taken from scripts/cross-common.sh
#   DRAFT=1       create the Release as a draft
#
# Requires: git, curl, python (any 3.x)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OWNER="${GITHUB_OWNER:-Lynx3211463259}"
REPO="${GITHUB_REPO:-ModelForge}"
VERSION="${VERSION:-$(tr -d '[:space:]' < "$ROOT/VERSION")}"
TAG="v${VERSION}"
API="https://api.github.com"
DRAFT_FLAG="false"
[ "${DRAFT:-0}" = "1" ] && DRAFT_FLAG="true"
# A hyphen in the version marks a pre-release (1.1.0-beta.1). PRERELEASE=1/0
# overrides the detection; pre-releases never show up as "latest" on GitHub.
PRERELEASE_FLAG="false"
case "$(printf '%s' "$VERSION" | tr '[:upper:]' '[:lower:]')" in
  *-*) PRERELEASE_FLAG="true" ;;
esac
if [ "${PRERELEASE:-auto}" = "1" ]; then PRERELEASE_FLAG="true"; fi
if [ "${PRERELEASE:-auto}" = "0" ]; then PRERELEASE_FLAG="false"; fi

log() { printf '\033[1;36m[publish]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

: "${GITHUB_TOKEN:?set GITHUB_TOKEN to a PAT with repo scope}"
command -v curl >/dev/null || die "curl is required"
command -v python >/dev/null || die "python is required"

AUTH=(
  -H "Authorization: Bearer ${GITHUB_TOKEN}"
  -H "Accept: application/vnd.github+json"
  -H "X-GitHub-Api-Version: 2022-11-28"
)

notes="$ROOT/docs/release-notes-${VERSION}.md"
[ -f "$notes" ] || die "missing release notes: $notes"
[ -f "$ROOT/release/SHA256SUMS.txt" ] || die "missing release/SHA256SUMS.txt"

# --- 1. repository -----------------------------------------------------------
code="$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" "$API/repos/$OWNER/$REPO")"
if [ "$code" = "404" ]; then
  log "creating repository $OWNER/$REPO"
  curl -fsS "${AUTH[@]}" -X POST "$API/user/repos" \
    -d "{\"name\":\"$REPO\",\"description\":\"Local AI Model Management, Conversion & Quantization Workstation\",\"private\":false}" \
    >/dev/null
elif [ "$code" != "200" ]; then
  die "cannot access $OWNER/$REPO (HTTP $code)"
else
  log "repository exists: $OWNER/$REPO"
fi

# --- 2. push ----------------------------------------------------------------
remote="https://github.com/$OWNER/$REPO.git"
auth_header="AUTHORIZATION: bearer ${GITHUB_TOKEN}"
git remote get-url origin >/dev/null 2>&1 || git remote add origin "$remote"

log "pushing main"
git -c http.extraheader="$auth_header" push "$remote" main
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
  log "pushing $TAG"
  git -c http.extraheader="$auth_header" push "$remote" "$TAG"
else
  die "tag $TAG does not exist locally; run: git tag -a $TAG -m 'ModelForge $VERSION'"
fi

# --- 3. release -------------------------------------------------------------
log "creating release $TAG"
# The release notes contain a marked downloads block. Regenerate it for the
# version being published so every release description links to its own assets,
# grouped by operating system (the layout used by llama.cpp releases).
body_file="$(mktemp)"
python - "$OWNER" "$REPO" "$VERSION" "$notes" "$body_file" <<'PY'
import sys

owner, repo, version, notes_path, out_path = sys.argv[1:6]
notes = open(notes_path, encoding="utf-8").read()
base = f"https://github.com/{owner}/{repo}/releases/download/v{version}"

groups = [
    ("Windows", [("Windows x64 — portable", "win64")]),
    ("macOS", [
        ("macOS Apple Silicon (arm64)", "macos-arm64"),
        ("macOS Intel (x64)", "macos-x64"),
    ]),
    ("Linux", [
        ("Linux x64 (static musl)", "linux-x64"),
        ("Linux arm64 (static musl)", "linux-arm64"),
    ]),
    ("Android", [
        ("Android arm64 (Termux)", "android-arm64"),
        ("Android x64 (Termux)", "android-x64"),
    ]),
    ("openEuler", [
        ("openEuler x64 (static musl)", "openeuler-x64"),
        ("openEuler arm64 (static musl)", "openeuler-arm64"),
    ]),
]

lines = [
    "Choose the archive for your operating system and architecture. All builds are ZIP files.",
    "",
]
for title, items in groups:
    lines.append(f"### {title}")
    lines.append("")
    for label, slug in items:
        lines.append(f"- [{label}]({base}/ModelForge-{version}-{slug}.zip)")
    lines.append("")
lines.extend([
    "iOS: no distributable build (signing required; no long-lived background HTTP service).",
    "Source stays syntax-checked only.",
    "",
    "Verify your download with `SHA256SUMS.txt` in the Assets section below.",
])
block = "\n".join(lines)
start, end = "<!-- DOWNLOADS:START -->", "<!-- DOWNLOADS:END -->"
if start in notes and end in notes:
    head, rest = notes.split(start, 1)
    _, tail = rest.split(end, 1)
    notes = head + start + "\n" + block + "\n" + end + tail
else:
    notes = notes.rstrip() + "\n\n## Downloads\n\n" + start + "\n" + block + "\n" + end + "\n"
open(out_path, "w", encoding="utf-8").write(notes)
PY
payload_file="$(mktemp)"
python - "$TAG" "$VERSION" "$body_file" "$DRAFT_FLAG" "$PRERELEASE_FLAG" "$payload_file" <<'PY'
import json, sys

tag, version, body_path, draft, prerelease, out_path = sys.argv[1:7]
payload = {
    "tag_name": tag,
    "name": f"ModelForge {version}",
    "body": open(body_path, encoding="utf-8").read(),
    "draft": draft == "true",
    "prerelease": prerelease == "true",
}
with open(out_path, "w", encoding="utf-8") as fh:
    json.dump(payload, fh)
PY
release_json="$(curl -fsS "${AUTH[@]}" -X POST "$API/repos/$OWNER/$REPO/releases" -d @"$payload_file")"
release_id="$(printf '%s' "$release_json" | python -c 'import json,sys; print(json.load(sys.stdin)["id"])')"
log "release id: $release_id"

# --- 4. assets --------------------------------------------------------------
shopt -s nullglob
assets=(
  release/ModelForge-"$VERSION"-*.zip
  release/SHA256SUMS.txt
)
[ "${#assets[@]}" -gt 0 ] || die "no assets matched release/ModelForge-$VERSION-*"

for f in "${assets[@]}"; do
  name="$(basename "$f")"
  log "uploading $name ($(du -h "$f" | cut -f1))"
  curl -fsS "${AUTH[@]}" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @"$f" \
    "$API/repos/$OWNER/$REPO/releases/$release_id/assets?name=$name" \
    >/dev/null
done

log "published: https://github.com/$OWNER/$REPO/releases/tag/$TAG"
