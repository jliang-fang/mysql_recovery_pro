#!/bin/bash
# pull_mysql_mirrors.sh - Pull latest MySQL images per series from Chinese mirrors
# Series: 5.6, 5.7, 8.0, 8.4, 9.0
# Strategy: try mirrors in order, retry on failure, then tag both exact and series tags.

set -u

MIRRORS=(
  "docker.1panel.live"
  "docker.1ms.run"
  "docker.m.daocloud.io"
  "docker.xuanyuan.me"
)

# series -> latest patch version
SERIES=("5.6" "5.7" "8.0" "8.4" "9.0")
declare -A VER=(
  ["5.6"]="5.6.51"
  ["5.7"]="5.7.44"
  ["8.0"]="8.0.46"
  ["8.4"]="8.4.11"
  ["9.0"]="9.0.1"
)

log() { echo "[$(date +%H:%M:%S)] $*"; }

FAILED=()
SUCCEEDED=()

for s in "${SERIES[@]}"; do
  v="${VER[$s]}"
  log "===== series ${s} -> target ${v} ====="
  pulled_src=""
  for m in "${MIRRORS[@]}"; do
    log "trying ${m}/library/mysql:${v}"
    if podman pull "${m}/library/mysql:${v}"; then
      pulled_src="${m}/library/mysql:${v}"
      log "OK pulled ${pulled_src}"
      break
    fi
    log "FAILED via ${m}, trying next mirror"
  done

  if [ -n "${pulled_src}" ]; then
    podman tag "${pulled_src}" "mysql:${v}"
    podman tag "${pulled_src}" "mysql:${s}"
    log "tagged: mysql:${v}  and  mysql:${s}"
    SUCCEEDED+=("${s}=${v}")
  else
    log "ALL MIRRORS FAILED for series ${s}"
    FAILED+=("${s}")
  fi
done

log "================ SUMMARY ================"
log "SUCCEEDED: ${SUCCEEDED[*]:-none}"
log "FAILED:    ${FAILED[*]:-none}"
podman images --format '{{.Repository}}:{{.Tag}}  {{.Size}}' | grep -E 'mysql' | sort
log "=============== DONE ==============="
