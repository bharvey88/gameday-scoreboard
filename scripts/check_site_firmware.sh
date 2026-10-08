#!/usr/bin/env bash
# Refuse to publish a site that would break updates: every variant's manifest
# must exist, match the release tag, and point only at files that are there.
# Usage: scripts/check_site_firmware.sh <site dir> <tag, e.g. v1.5.4>
set -euo pipefail

site=$1
want=${2#v}
fail=0

for v in moonhub75 scoreboard75; do
  m="$site/firmware/$v/manifest.json"
  if [ ! -f "$m" ]; then
    echo "::error::$m is missing"
    fail=1
    continue
  fi
  bad=0
  got=$(jq -r .version "$m")
  if [ "$got" != "$want" ]; then
    echo "::error::$m is version $got, the release is $want"
    bad=1
  fi
  for f in $(jq -r '.builds[] | (.ota.path // empty), (.parts[]?.path // empty)' "$m"); do
    if [ ! -f "$site/firmware/$v/$f" ]; then
      echo "::error::$m points at $f, which is not in the site"
      bad=1
    fi
  done
  if [ $bad = 0 ]; then
    echo "$v: manifest $got, all files present"
  fi
  fail=$((fail | bad))
done

exit $fail
