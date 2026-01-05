#!/bin/bash

# Count unique SASS instructions from a .sass file
# Usage: ./count_sass_instructions.sh [file.sass]

SASS_FILE="${1:-ThingNVSHMEM.sass}"

if [[ ! -f "$SASS_FILE" ]]; then
    echo "Error: File '$SASS_FILE' not found"
    exit 1
fi

echo "=== SASS Instruction Count for: $SASS_FILE ==="
echo

# Count unique instructions
UNIQUE_COUNT=$(grep -oP '\/\*[0-9a-f]+\*\/\s+(@[!]?P\d+\s+)?[A-Z][A-Z0-9_.]+' "$SASS_FILE" | \
    grep -oP '[A-Z][A-Z0-9_.]+$' | \
    sort -u | wc -l)

echo "Total unique instructions: $UNIQUE_COUNT"
echo
echo "=== Frequency breakdown (sorted by count) ==="
echo

grep -oP '\/\*[0-9a-f]+\*\/\s+(@[!]?P\d+\s+)?[A-Z][A-Z0-9_.]+' "$SASS_FILE" | \
    grep -oP '[A-Z][A-Z0-9_.]+$' | \
    sort | uniq -c | sort -rn
