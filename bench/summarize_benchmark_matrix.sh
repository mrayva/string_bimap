#!/usr/bin/env bash
set -euo pipefail

in_dir="${1:-bench/results}"
csv_out="${2:-${in_dir}/summary.csv}"
md_out="${3:-${in_dir}/summary.md}"

if [[ ! -d "${in_dir}" ]]; then
  echo "input directory not found: ${in_dir}" >&2
  exit 1
fi

extract_ns() {
  local file="$1"
  local metric="$2"
  awk -v metric="${metric}" '
    $1 == metric {
      for (i = 1; i <= NF; ++i) {
        if ($i ~ /^ns_per_op=/) {
          sub(/^ns_per_op=/, "", $i)
          print $i
          exit
        }
      }
    }
  ' "${file}"
}

extract_bytes() {
  local file="$1"
  local key="$2"
  awk -F= -v key="${key}" '$1 == key { print $2; exit }' "${file}"
}

extract_scalar() {
  local file="$1"
  local key="$2"
  awk -F= -v key="${key}" '$1 == key { print $2; exit }' "${file}"
}

tmp_csv="$(mktemp)"
{
  echo "dataset,profile,insert_ns,find_id_ns,get_string_ns,load_ns,steady_loaded_ns,internal_after_load_total_bytes,compact_ns,mixed_load_ns,compacted_load_ns"
  shopt -s nullglob
  for file in "${in_dir}"/*.txt; do
    base="$(basename "${file}" .txt)"
    dataset="${base%_*}"
    profile="${base##*_}"
    insert_ns="$(extract_ns "${file}" insert || true)"
    find_ns="$(extract_ns "${file}" find_id || true)"
    get_ns="$(extract_ns "${file}" get_string || true)"
    load_ns="$(extract_ns "${file}" load || true)"
    steady_ns="$(extract_ns "${file}" steady_loaded || true)"
    compact_ns="$(extract_ns "${file}" compact || true)"
    mixed_load_ns="$(extract_ns "${file}" mixed_load || true)"
    compacted_load_ns="$(extract_ns "${file}" compacted_load || true)"
    internal_load="$(extract_bytes "${file}" internal_after_load_total_bytes || true)"
    echo "${dataset},${profile},${insert_ns},${find_ns},${get_ns},${load_ns},${steady_ns},${internal_load},${compact_ns},${mixed_load_ns},${compacted_load_ns}"
  done | sort
} > "${tmp_csv}"

mv "${tmp_csv}" "${csv_out}"

{
  echo "| Dataset | Profile | Insert ns/op | Find ns/op | Get ns/op | Load ns/op | Steady Loaded ns/op | Internal After Load | Compact ns/op |"
  echo "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
  tail -n +2 "${csv_out}" | while IFS=, read -r dataset profile insert_ns find_ns get_ns load_ns steady_ns internal_load compact_ns mixed_load_ns compacted_load_ns; do
    echo "| ${dataset} | ${profile} | ${insert_ns:-} | ${find_ns:-} | ${get_ns:-} | ${load_ns:-} | ${steady_ns:-} | ${internal_load:-} | ${compact_ns:-} |"
  done
} > "${md_out}"

echo "wrote ${csv_out}"
echo "wrote ${md_out}"
