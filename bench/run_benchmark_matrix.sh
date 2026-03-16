#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build-vcpkg}"
out_dir="${2:-bench/results}"

mkdir -p "${out_dir}"

bench="${build_dir}/string_bimap_bench"
if [[ ! -x "${bench}" ]]; then
  echo "benchmark executable not found: ${bench}" >&2
  echo "build it first, e.g. cmake --build ${build_dir} --target string_bimap_bench" >&2
  exit 1
fi

profiles=(fast array_map marisa marisa_array_map)

run_case() {
  local name="$1"
  shift
  for profile in "${profiles[@]}"; do
    local out_file="${out_dir}/${name}_${profile}.txt"
    echo "running ${name} / ${profile} -> ${out_file}"
    "${bench}" --profile "${profile}" "$@" > "${out_file}"
  done
}

run_case synthetic \
  --keys 50000 \
  --prefix-groups 256 \
  --phases insert,find,get,erase,compact,save,load,steady_loaded \
  --serialized-file /tmp/string_bimap_matrix_synthetic.bin \
  --read-repeats 3 \
  --loaded-find-ratio 4 \
  --loaded-get-ratio 1 \
  --seed 7

if [[ -f /tmp/StockETFList ]]; then
  run_case stocketf_symbol \
    --csv /tmp/StockETFList \
    --column Symbol \
    --phases insert,find,get,erase,compact,save,load,steady_loaded \
    --serialized-file /tmp/string_bimap_matrix_stocketf_symbol.bin \
    --read-repeats 3 \
    --loaded-find-ratio 4 \
    --loaded-get-ratio 1

  run_case stocketf_company_name \
    --csv /tmp/StockETFList \
    --column "Company Name" \
    --phases insert,find,get,erase,compact,save,load,steady_loaded \
    --serialized-file /tmp/string_bimap_matrix_stocketf_company_name.bin \
    --read-repeats 3 \
    --loaded-find-ratio 4 \
    --loaded-get-ratio 1
fi

if [[ -f /tmp/CUSIP.csv ]]; then
  run_case cusip_symbol \
    --csv /tmp/CUSIP.csv \
    --column symbol \
    --phases insert,find,get,erase,compact,save,load,steady_loaded \
    --serialized-file /tmp/string_bimap_matrix_cusip_symbol.bin \
    --read-repeats 3 \
    --loaded-find-ratio 4 \
    --loaded-get-ratio 1

  run_case cusip_description \
    --csv /tmp/CUSIP.csv \
    --column description \
    --phases insert,find,get,erase,compact,save,load,steady_loaded \
    --serialized-file /tmp/string_bimap_matrix_cusip_description.bin \
    --read-repeats 3 \
    --loaded-find-ratio 4 \
    --loaded-get-ratio 1
fi

if [[ -f /tmp/SEC_CIKs_Symbols.csv ]]; then
  run_case sec_symbol \
    --csv /tmp/SEC_CIKs_Symbols.csv \
    --column symbol \
    --phases insert,find,get,erase,compact,save,load,steady_loaded \
    --serialized-file /tmp/string_bimap_matrix_sec_symbol.bin \
    --read-repeats 3 \
    --loaded-find-ratio 4 \
    --loaded-get-ratio 1
fi

if [[ -f /tmp/naskitis/distinct_1 && -f /tmp/naskitis/skew1_1 ]]; then
  run_case naskitis \
    --line-file-write /tmp/naskitis/distinct_1 \
    --line-file-read /tmp/naskitis/skew1_1 \
    --read-limit 1000000 \
    --phases insert,find,get,save,load,steady_loaded \
    --serialized-file /tmp/string_bimap_matrix_naskitis.bin \
    --read-repeats 2 \
    --loaded-find-ratio 4 \
    --loaded-get-ratio 1
fi

if [[ -f /tmp/enwiki-latest-all-titles-in-ns0.keys.txt ]]; then
  run_case wikipedia \
    --line-file /tmp/enwiki-latest-all-titles-in-ns0.keys.txt \
    --shuffle \
    --phases insert,erase,compare_snapshots \
    --serialized-file /tmp/string_bimap_matrix_enwiki.bin \
    --read-repeats 3 \
    --loaded-find-ratio 4 \
    --loaded-get-ratio 1 \
    --seed 7
fi

echo "benchmark matrix completed in ${out_dir}"
