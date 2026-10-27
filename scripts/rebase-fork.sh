#!/usr/bin/env bash
# Rebase the fork's patch series (PATCHES.md) onto a fresh upstream/main.
#
#   scripts/rebase-fork.sh [-n] [-s SERIES_BRANCH] [-b BASE_REF] [-o BUILD_DIR] [-t "hyprtester test names"]
#
#   -s  branch carrying the series (default: the current branch)
#   -b  upstream ref to rebase onto (default: upstream/main, fetched first)
#   -o  build dir (default: /tmp/hyprland-rebase-<date>); the build is configured with WITH_TESTS=ON
#   -t  hyprtester subset (default: a group / rule subset); "" skips hyprtester
#   -n  dry run: print the plan, change nothing
#
# Steps: fetch upstream, tag the series branch as backup/pre-rebase-<date>, create <series>-<date> from the base,
# cherry-pick every commit of the series (base..series) in order, configure + build into BUILD_DIR, run the
# solid_glass unit tests and the full gtest suite, run a hyprtester subset. Stops at the first conflict or failure
# and leaves the new branch checked out for fixing. The series branch itself is never moved or pushed.
#
# PKG_CONFIG_PATH is passed through (needed while hyprland-protocols >= 0.8.0 is not installed system-wide, see
# NOTES.md). Run from a clean worktree.
set -euo pipefail

DATE=$(date +%Y%m%d)
SERIES=$(git rev-parse --abbrev-ref HEAD)
BASE=upstream/main
BUILD=""
TESTS="groupNext groupMoveWindow groupbarMiddleClick groupsNoCrash groups_disable_when_only groupbarDisableWhenOnlyDimensions windowGroupRules contentWindowRules"
DRY=0

while getopts "ns:b:o:t:" opt; do
    case $opt in
        n) DRY=1 ;;
        s) SERIES=$OPTARG ;;
        b) BASE=$OPTARG ;;
        o) BUILD=$OPTARG ;;
        t) TESTS=$OPTARG ;;
        *) sed -n '2,20p' "$0"; exit 2 ;;
    esac
done
BUILD=${BUILD:-/tmp/hyprland-rebase-$DATE}
NEW="$SERIES-$DATE"
BACKUP="backup/pre-rebase-$DATE"
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

run() {
    echo "+ $*"
    [ "$DRY" = 1 ] || "$@"
}

[ "$DRY" = 1 ] || [ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "worktree has tracked changes, commit or stash them first" >&2; exit 1; }

case $BASE in upstream/*) [ "$DRY" = 1 ] || git fetch upstream ;; esac

OLD_BASE=$(git merge-base "$BASE" "$SERIES")
mapfile -t COMMITS < <(git rev-list --reverse --no-merges "$(git merge-base upstream/main "$SERIES")..$SERIES")
echo "series $SERIES: ${#COMMITS[@]} commits on top of $(git log -1 --format='%h %s' "$OLD_BASE")"
for c in "${COMMITS[@]}"; do git log -1 --format='  %h %s' "$c"; done
echo "new base: $(git log -1 --format='%h %s' "$BASE")"

if git rev-parse -q --verify "refs/heads/$BACKUP" > /dev/null || git rev-parse -q --verify "refs/tags/$BACKUP" > /dev/null; then
    echo "$BACKUP already exists, not overwriting it" >&2
else
    run git branch "$BACKUP" "$SERIES"
fi
run git switch -c "$NEW" "$BASE"
for c in "${COMMITS[@]}"; do
    if ! run git cherry-pick -x "$c"; then
        echo "conflict on $(git log -1 --format='%h %s' "$c"): resolve, 'git cherry-pick --continue', then re-run the remaining picks" >&2
        exit 1
    fi
done

run mkdir -p "$BUILD"
run cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DWITH_TESTS=ON
run cmake --build "$BUILD" -j"$(nproc)" --target Hyprland hyprctl hyprland_gtests hyprtester

run "$BUILD/hyprland_gtests" --gtest_filter='SolidGlass*:Config.*'
run "$BUILD/hyprland_gtests"

if [ -n "$TESTS" ]; then
    # hyprtester runs its own headless instances; keep them off the live session's runtime dir
    RT=$(mktemp -d /tmp/hrt.XXXX)
    (cd "$ROOT/hyprtester" && run env -u HYPRLAND_INSTANCE_SIGNATURE XDG_RUNTIME_DIR="$RT" "$BUILD/hyprtester/hyprtester" -b "$BUILD/Hyprland" \
        -p plugin/hyprtestplugin.so -c test.lua $TESTS)
fi

echo "done: $NEW built in $BUILD, old series kept as $BACKUP. Install / push stay manual (NOTES.md)."
