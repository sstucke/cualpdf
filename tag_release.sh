#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Error: este script debe ejecutarse dentro del repo git."
    exit 1
fi

if [ $# -ne 1 ]; then
    echo "Uso: ./tag_release.sh <version>"
    echo "Ejemplos:"
    echo "  ./tag_release.sh 1.0.17"
    echo "  ./tag_release.sh v1.0.17"
    exit 1
fi

version="$1"
tag="${version#v}"
tag="v${tag}"

if ! [[ "$tag" =~ ^v[0-9]+(\.[0-9]+)*$ ]]; then
    echo "Error: versión inválida '$version'. Usá algo como 1.0.17 o v1.0.17."
    exit 1
fi

if git rev-parse -q --verify "refs/tags/${tag}" >/dev/null 2>&1; then
    echo "Error: el tag ${tag} ya existe localmente."
    exit 1
fi

if git ls-remote --tags origin "refs/tags/${tag}" | grep -q .; then
    echo "Error: el tag ${tag} ya existe en origin."
    exit 1
fi

git tag "${tag}"
git push origin "${tag}"

echo "Tag publicado: ${tag}"
echo "Build en GitHub Actions: https://github.com/sstucke/cualpdf/actions"
