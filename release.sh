#!/bin/bash

set -euo pipefail

mkdir -p out

for key in $(yq -rM 'keys | .[]' categories.yaml); do
  val=$(yq -rM ".${key}" categories.yaml)
  
  echo "Parsing exam ${key}..."
  ./exam "$val" "out/${key}.zip"
done
