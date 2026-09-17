#!/usr/bin/env bash
set -euo pipefail

# Configuration
THRESHOLD_BYTES=10485760 # Example: 10MB limit
MASSIF_OUT="massif.out.%p"

# Run application under Massif
# Note: Use your existing suppressions to avoid noise in the heap profile
valgrind --tool=massif \
         --massif-out-file="$MASSIF_OUT" \
         --suppressions=valgrind.supp \
         ./your_app_binary

# Find the generated massif output file
MASSIF_FILE=$(ls massif.out.* | head -n 1)

# Extract peak heap usage (in bytes)
# The peak is the first value in the 'mem_heap_B' field of the peak snapshot
PEAK_BYTES=$(grep -m 1 "mem_heap_B=" "$MASSIF_FILE" | cut -d'=' -f2)

echo "Peak heap usage: $PEAK_BYTES bytes"

# Verify against threshold
if [ "$PEAK_BYTES" -gt "$THRESHOLD_BYTES" ]; then
    echo "ERROR: Heap usage exceeded limit of $THRESHOLD_BYTES bytes."
    exit 1
fi