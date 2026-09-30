#!/usr/bin/env bash
# Install the LumenCode Claude Code skill from this checkout. Run manually:
#   tools/agent/install.sh [--copy]
set -euo pipefail

agent_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
skill_dir="${HOME}/.claude/skills"
target="${skill_dir}/lumencode"

mkdir -p "${skill_dir}"
if [[ ${1:-} == "--copy" ]]; then
    rm -rf "${target}"
    cp -R "${agent_dir}" "${target}"
    printf 'Installed a copy at %s (rerun after checkout updates).\n' "${target}"
else
    ln -sfn "${agent_dir}" "${target}"
    printf 'Installed a symlink at %s (it follows checkout updates).\n' "${target}"
fi
printf 'Hook settings template: %s/hooks/settings.example.json\n' "${target}"
