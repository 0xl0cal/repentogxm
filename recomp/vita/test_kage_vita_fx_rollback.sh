#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
runtime_dir="$(cd "${script_dir}/../runtime" && pwd)"
source_file="${runtime_dir}/kage_vita_fx_rollback.c"
oracle_file="${runtime_dir}/kage_vita_fx_rollback_oracle.c"
cc_bin="${CC:-cc}"
tmp_dir="$(mktemp -d)"
trap 'rm -rf -- "${tmp_dir}"' EXIT

if grep -En '\b(malloc|calloc|realloc|free|longjmp|guest_fault)[[:space:]]*\(' \
     "${source_file}"; then
  echo "FXLayers rollback helper owns a forbidden allocation/fault edge" >&2
  exit 1
fi

common_flags=(-std=c11 -Wall -Wextra -Werror -I"${runtime_dir}")
"${cc_bin}" "${common_flags[@]}" -DORACLE_ENABLE_POLICY=0 \
  "${oracle_file}" -o "${tmp_dir}/fx-rollback-off"
"${tmp_dir}/fx-rollback-off"

"${cc_bin}" "${common_flags[@]}" -DORACLE_ENABLE_POLICY=1 \
  "${oracle_file}" -o "${tmp_dir}/fx-rollback-on"
"${tmp_dir}/fx-rollback-on"

if [[ -n "${VITASDK:-}" ]]; then
  arm_cc="${VITASDK}/bin/arm-vita-eabi-gcc"
  arm_nm="${VITASDK}/bin/arm-vita-eabi-nm"
  arm_flags=(-std=gnu11 -O2 -Wall -Wextra -Werror
    -DGUEST_STACK_REQUIRED=1 -mcpu=cortex-a9 -mfpu=neon
    -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections
    -I"${runtime_dir}")
  "${arm_cc}" "${arm_flags[@]}" -DISAAC_VITA_FXLAYERS_NULL_ROLLBACK=1 \
    -c "${source_file}" -o "${tmp_dir}/fx-rollback-arm-on.o"
  "${arm_cc}" "${arm_flags[@]}" \
    -c "${source_file}" -o "${tmp_dir}/fx-rollback-arm-off.o"
  "${arm_nm}" -u "${tmp_dir}/fx-rollback-arm-on.o" \
    > "${tmp_dir}/fx-rollback-arm-on.nm"
  grep -q 'isaac_vita_guest_heap_lease_containing' \
    "${tmp_dir}/fx-rollback-arm-on.nm"
  grep -q 'isaac_vita_guest_heap_lease_release' \
    "${tmp_dir}/fx-rollback-arm-on.nm"
  if grep -Eq '(__atomic|malloc_usable_size|guest_heap_owns)' \
       "${tmp_dir}/fx-rollback-arm-on.nm"; then
    echo "FXLayers rollback ARM object gained a forbidden dependency" >&2
    exit 1
  fi
  if "${arm_nm}" -u "${tmp_dir}/fx-rollback-arm-off.o" | \
       grep -Eq 'isaac_vita_guest_heap_(lease|owns)'; then
    echo "default-OFF ARM object retained a heap-policy dependency" >&2
    exit 1
  fi
  echo "FXLayers rollback ARM softfp object gate PASS"
fi

echo "FXLayers rollback host oracle PASS"
