#!/usr/bin/env bash
set -euo pipefail

out_dir="${1:-/tmp/openfigi-values}"
mkdir -p "${out_dir}"

base_url="https://api.openfigi.com/v3/mapping/values"
keys=(
  idType
  exchCode
  micCode
  currency
  marketSecDes
  securityType
  securityType2
  stateCode
)

for key in "${keys[@]}"; do
  out_file="${out_dir}/${key}.json"
  echo "fetching ${key} -> ${out_file}"
  curl -L "${base_url}/${key}" -o "${out_file}"
done

echo "fetched OpenFIGI value sets into ${out_dir}"
