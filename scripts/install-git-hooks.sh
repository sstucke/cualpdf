#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

if [ ! -d "$root/.git" ]; then
    echo "scripts/install-git-hooks.sh: no hay un repositorio git en $root" >&2
    exit 1
fi

git -C "$root" config core.hooksPath .githooks
chmod +x "$root/.githooks/"* "$root/scripts/check-ai-attribution.sh" "$root/scripts/install-git-hooks.sh"
echo "cualpdf: core.hooksPath=.githooks"
