#!/usr/bin/env bash
source "$(dirname "$0")/lib.sh"

install_files() {
    ensure_directory "/opt/app"
    cp -r ./dist /opt/app
}

install_files
