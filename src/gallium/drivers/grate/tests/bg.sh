#!/bin/bash
# bg.sh <unit-name> <logfile> <command...>  -- run detached, survives ssh logout
unit="$1"; log="$2"; shift 2
echo samuca | sudo -S systemctl reset-failed "$unit" 2>/dev/null
echo samuca | sudo -S systemd-run --unit="$unit" --collect --uid=1000 --gid=1000 \
  --property=SupplementaryGroups='video render input seat' \
  --setenv=HOME=/home/sam --setenv=PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin \
  --working-directory=/home/sam/Dev \
  bash -c "$* > $log 2>&1" 2>&1 | grep -v "^\[sudo" 
