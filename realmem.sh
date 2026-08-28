#!/bin/bash
# Rank memory by phys_footprint (excludes OS file cache), aggregated by app.
# No root required.
top -l 1 -o mem -n 200 -stats mem,command 2>/dev/null | tail -n +13 | awk '
function b(v) {
  u = substr(v, length(v)); n = substr(v, 1, length(v)-1)
  if (u=="G") return n*1024; if (u=="M") return n+0
  if (u=="K") return n/1024; if (u=="B") return 0
  return n/1048576
}
{ mb = b($1); name = $2
  for (i=3; i<=NF; i++) name = name " " $i
  sub(/ Helper.*/, "", name); sub(/ Web Content.*/, "", name)
  sub(/^com\.apple\.WebKit.*/, "WebKit (Safari)", name)
  gsub(/[0-9]+$/, "", name)
  m[name] += mb; c[name]++ }
END { for (k in m) printf "%8.0f MB  %3d  %s\n", m[k], c[k], k }
' | sort -rn | head -20
echo "---"
top -l 1 -n 0 -s 0 | grep PhysMem
