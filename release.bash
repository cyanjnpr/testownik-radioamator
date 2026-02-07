#!/bin/bash

set -euo pipefail

mkdir out

for key in $(yq -rM 'keys | .[]' categories.yaml); do
  val=$(yq -rM ".${key}" categories.yaml)
  
  ./exam "$val" "out/${key}.zip"
done
