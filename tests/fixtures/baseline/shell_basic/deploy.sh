#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"
export DEPLOY_ENV=production

log() {
    printf '%s %s\n' "$(date +%T)" "$*"
}

function build_image {
    local tag="$1"
    local context="${2:-.}"
    log "building ${tag}"
    docker build -t "$tag" "$context" | awk '{ print $1 }'
}

deploy() {
    local tag="$1"
    build_image "$tag" && log "deployed $(summary "$tag")"
}

summary() {
    echo "tag=$1"
}

deploy "${1:-latest}"
