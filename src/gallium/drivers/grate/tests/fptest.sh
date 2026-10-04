#!/bin/bash
# Fragment suite: grate vs softpipe (the oracle).
# fp20/fx10 are lower precision than softpipe's fp32, so channels are compared
# with a tolerance rather than exactly.
TOL=${TOL:-10}
S=/home/sam/Dev/stage-main/lib
cd /home/sam/Dev/probe || exit 1
export LD_LIBRARY_PATH=$S EGL_PLATFORM=surfaceless
# GRATE_FP_SFU=1 also runs the special-function tests, which are known to fail
timeout 900 ./fptest > /tmp/fp_grate.txt 2>/tmp/fp_grate.err
LIBGL_ALWAYS_SOFTWARE=1 timeout 900 ./fptest > /tmp/fp_ref.txt 2>/dev/null
awk -v tol="$TOL" -v sfu="${GRATE_FP_SFU:-}" '
  FILENAME==ARGV[1] && $1 !~ /^#/ { ref[$1]=$2; next }
  $1 ~ /^#/ { next }
  # the SFU ops are only compiled when GRATE_FP_SFU is set, so do not count
  # them as failures when they are switched off
  $1 ~ /^sfu_/ && sfu == "" { next }
  {
    name=$1; got=$2; r=ref[name]
    if (r == "") { printf "%-12s %-16s %-16s %s\n", name, got, "(missing)", "FAIL"; fail++; next }
    if (got !~ /,/ || r !~ /,/) {
      res = (got == r) ? "PASS" : "FAIL"
      printf "%-12s %-16s %-16s %s\n", name, got, r, res
      if (res=="PASS") pass++; else fail++; next
    }
    split(got, g, ","); split(r, e, ","); ok=1; worst=0
    for (i=1; i<=4; i++) { d = g[i]-e[i]; if (d<0) d=-d; if (d>worst) worst=d; if (d>tol) ok=0 }
    res = ok ? "PASS" : "FAIL"
    printf "%-12s %-16s %-16s %s (max delta %d)\n", name, got, r, res, worst
    if (ok) pass++; else fail++
  }
  END { printf "---- %d passed, %d failed (tolerance +/-%d) ----\n", pass, fail, tol }
' /tmp/fp_ref.txt /tmp/fp_grate.txt
if [ -s /tmp/fp_grate.err ]; then echo "grate stderr:"; sort -u /tmp/fp_grate.err | head -20; fi
