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
VERSION="${VERSION:-$(grep -m1 '^VERSION=' scripts/cross-common.sh | cut -d'"' -f2)}"
TAG="v${VERSION}"
API="https://api.github.com"
DRAFT_FLAG="false"
[ "${DRAFT:-0}" = "1" ] && DRAFT_FLAG="true"

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
release_json="$(curl -fsS "${AUTH[@]}" -X POST "$API/repos/$OWNER/$REPO/releases" \
  -d "$(python - "$TAG" "$VERSION" "$notes" "$DRAFT_FLAG" <<'PY'
import json, sys
tag, version, path, draft = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
body = open(path, encoding="utf-8").read()
print(json.dumps({
    "tag_name": tag,
    "name": f"ModelForge {version}",
    "body": body,
    "draft": draft == "true",
    "prerelease": False,
}))
PY
)")"
release_id="$(printf '%s' "$release_json" | python -c 'import json,sys; print(json.load(sys.stdin)["id"])')"
log "release id: $release_id"

# --- 4. assets --------------------------------------------------------------
shopt -s nullglob
assets=(
  release/ModelForge-"$VERSION"-*.zip
  release/ModelForge-"$VERSION"-*.tar.gz
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
