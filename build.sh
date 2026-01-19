#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Optionally source emsdk env when EMSDK_ENV points at emsdk_env.sh.
if [[ -n "${EMSDK_ENV:-}" && -f "${EMSDK_ENV}" ]]; then
  # shellcheck disable=SC1090
  source "${EMSDK_ENV}"
fi

if ! command -v emcc >/dev/null 2>&1; then
  echo "emcc not found; source emsdk_env.sh or set EMSDK_ENV=/path/to/emsdk_env.sh" >&2
  exit 1
fi
if ! command -v emmake >/dev/null 2>&1; then
  echo "emmake not found; source emsdk_env.sh or set EMSDK_ENV=/path/to/emsdk_env.sh" >&2
  exit 1
fi

OUT="${OUT:-}"
BC_FILE="${BC_FILE:-${ROOT_DIR}/melonds_libretro_emscripten.bc}"
MAKE_ARGS="${MAKE_ARGS:-}"
EMCC_ARGS="${EMCC_ARGS:-}"
BUILD_CORE_JS="${BUILD_CORE_JS:-1}"
BUILD_RETROARCH="${BUILD_RETROARCH:-1}"
if [[ "${BUILD_RETROARCH}" != "1" ]]; then
  echo "ERROR: BUILD_RETROARCH is locked to 1 in this script." >&2
  exit 1
fi
BUILD_RETROARCH=1
LIBRETRO_NAME="${LIBRETRO_NAME:-melonds}"
RETROARCH_DIR="${RETROARCH_DIR:-${ROOT_DIR}/retroarch-linker}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/dist-wasm}"
RA_MAKE_ARGS="${RA_MAKE_ARGS:-HAVE_AL=0 HAVE_CHEEVOS=1}"
RA_TARGET="${RA_TARGET:-${LIBRETRO_NAME}.js}"
ENABLE_PTHREADS="${ENABLE_PTHREADS:-0}"
PTHREAD_POOL_SIZE="${PTHREAD_POOL_SIZE:-}"
RA_JOBS="${RA_JOBS:-}"
RA_CLEAN="${RA_CLEAN:-1}"
RETROARCH_REPO="${RETROARCH_REPO:-https://github.com/libretro/RetroArch}"
RETROARCH_REF="${RETROARCH_REF:-}"
STATIC_LINKING="${STATIC_LINKING:-}"

RA_JOBS_ARG=""
if [[ -n "${RA_JOBS}" ]]; then
  RA_JOBS_ARG="-j${RA_JOBS}"
fi

normalize_bool() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) echo "1" ;;
    *) echo "0" ;;
  esac
}

sanitize_make_args() {
  local input="${1:-}"
  local output=""
  set -- ${input}
  while [[ $# -gt 0 ]]; do
    local token="$1"
    shift
    case "${token}" in
      HAVE_THREADS=*|PTHREAD_POOL_SIZE=*) continue ;;
    esac
    if [[ -n "${output}" ]]; then
      output+=" "
    fi
    output+="${token}"
  done
  printf "%s" "${output}"
}

sanitize_flag_list() {
  local input="${1:-}"
  local output=""
  set -- ${input}
  while [[ $# -gt 0 ]]; do
    local token="$1"
    shift
    if [[ "${token}" == "-pthread" ]]; then
      continue
    fi
    if [[ "${token}" == "-s" && $# -gt 0 ]]; then
      local next="$1"
      if [[ "${next}" == "SHARED_MEMORY" || "${next}" == PTHREAD_POOL_SIZE=* ]]; then
        shift
        continue
      fi
    fi
    if [[ -n "${output}" ]]; then
      output+=" "
    fi
    output+="${token}"
  done
  printf "%s" "${output}"
}

ENABLE_PTHREADS="$(normalize_bool "${ENABLE_PTHREADS}")"
MAKE_ARGS="$(sanitize_make_args "${MAKE_ARGS}")"
RA_MAKE_ARGS="$(sanitize_make_args "${RA_MAKE_ARGS}")"
EMCC_ARGS="$(sanitize_flag_list "${EMCC_ARGS}")"

clean_cflags="$(sanitize_flag_list "${CFLAGS:-}")"
clean_cxxflags="$(sanitize_flag_list "${CXXFLAGS:-}")"
clean_ldflags="$(sanitize_flag_list "${LDFLAGS:-}")"

MAKE_ENV=()
if [[ "${ENABLE_PTHREADS}" == "1" ]]; then
  PTHREAD_POOL_SIZE="${PTHREAD_POOL_SIZE:-4}"
  pthreads_cflags="-pthread -s SHARED_MEMORY"
  pthreads_ldflags="-pthread -s SHARED_MEMORY -s PTHREAD_POOL_SIZE=${PTHREAD_POOL_SIZE}"
  MAKE_ARGS="${MAKE_ARGS} HAVE_THREADS=1"
  EMCC_ARGS="${EMCC_ARGS} ${pthreads_ldflags}"
  RA_MAKE_ARGS="${RA_MAKE_ARGS} HAVE_THREADS=1 PTHREAD_POOL_SIZE=${PTHREAD_POOL_SIZE}"
  clean_cflags="${clean_cflags:+${clean_cflags} }${pthreads_cflags}"
  clean_cxxflags="${clean_cxxflags:+${clean_cxxflags} }${pthreads_cflags}"
  clean_ldflags="${clean_ldflags:+${clean_ldflags} }${pthreads_ldflags}"
else
  PTHREAD_POOL_SIZE=""
  MAKE_ARGS="${MAKE_ARGS} HAVE_THREADS=0"
  RA_MAKE_ARGS="${RA_MAKE_ARGS} HAVE_THREADS=0"
fi
MAKE_ENV+=("CFLAGS=${clean_cflags}")
MAKE_ENV+=("CXXFLAGS=${clean_cxxflags}")
MAKE_ENV+=("LDFLAGS=${clean_ldflags}")

if [[ -z "${STATIC_LINKING}" ]]; then
  if [[ "${BUILD_RETROARCH}" != "0" ]]; then
    STATIC_LINKING=1
  else
    STATIC_LINKING=0
  fi
fi

if [[ "${BUILD_RETROARCH}" != "0" ]]; then
  if [[ -z "${OUTPUT_DIR}" || "${OUTPUT_DIR}" == "/" ]]; then
    echo "ERROR: refusing to clean OUTPUT_DIR='${OUTPUT_DIR}'" >&2
    exit 1
  fi
  if [[ -d "${OUTPUT_DIR}" ]]; then
    find "${OUTPUT_DIR}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
  fi
fi

echo "Build config:"
echo "  BUILD_RETROARCH=${BUILD_RETROARCH}"
echo "  ENABLE_PTHREADS=${ENABLE_PTHREADS}"
echo "  MAKE_ARGS=${MAKE_ARGS}"
echo "  RA_MAKE_ARGS=${RA_MAKE_ARGS}"
echo "  RA_TARGET=${RA_TARGET}"
echo "  EMCC_ARGS=${EMCC_ARGS}"
if [[ "${ENABLE_PTHREADS}" == "1" ]]; then
  echo "  PTHREAD_POOL_SIZE=${PTHREAD_POOL_SIZE}"
else
  echo "  PTHREAD_POOL_SIZE=disabled"
fi
echo "  OUTPUT_DIR=${OUTPUT_DIR}"

pushd "${ROOT_DIR}" >/dev/null

make_clean_cmd=(emmake make -f Makefile platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib clean STATIC_LINKING="${STATIC_LINKING}" ${MAKE_ARGS})
make_build_cmd=(emmake make -f Makefile platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib STATIC_LINKING="${STATIC_LINKING}" ${MAKE_ARGS})
env "${MAKE_ENV[@]}" "${make_clean_cmd[@]}"
env "${MAKE_ENV[@]}" "${make_build_cmd[@]}"

if [[ "${BUILD_CORE_JS}" != "0" ]]; then
  if [[ -z "${OUT}" ]]; then
    OUT="${ROOT_DIR}/melonds.js"
  fi
  emcc "${BC_FILE}" -o "${OUT}" \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s EXPORT_NAME=melonds \
    -s EXPORT_ALL=1 \
    -s FORCE_FILESYSTEM=1 \
    -s EXPORTED_RUNTIME_METHODS=FS,PATH,ERRNO_CODES \
    ${EMCC_ARGS}
  echo "Built core JS: ${OUT}"
fi

popd >/dev/null

if [[ "${BUILD_RETROARCH}" != "0" ]]; then
  if [[ ! -d "${RETROARCH_DIR}" ]]; then
    if [[ -z "${RETROARCH_REPO}" ]]; then
      echo "ERROR: retroarch-linker directory missing at ${RETROARCH_DIR}" >&2
      echo "Set RETROARCH_REPO to auto-clone, or provide RETROARCH_DIR." >&2
      exit 1
    fi
    echo "Cloning retroarch-linker from ${RETROARCH_REPO}..."
    git clone --depth 1 "${RETROARCH_REPO}" "${RETROARCH_DIR}"
    if [[ -n "${RETROARCH_REF}" ]]; then
      git -C "${RETROARCH_DIR}" checkout "${RETROARCH_REF}"
    fi
  fi

  ra_target_base="${RA_TARGET%.js}"
  if [[ "${ENABLE_PTHREADS}" != "1" ]]; then
    rm -f "${RETROARCH_DIR}/${ra_target_base}.worker.js" "${RETROARCH_DIR}/${LIBRETRO_NAME}_libretro.worker.js"
  fi

  mkdir -p "${OUTPUT_DIR}"
  cp "${BC_FILE}" "${RETROARCH_DIR}/libretro_emscripten.bc"
  cp "${BC_FILE}" "${RETROARCH_DIR}/libretro_emscripten.a"

  pushd "${RETROARCH_DIR}" >/dev/null
  if [[ "${RA_CLEAN}" != "0" ]]; then
    emmake make -f Makefile.emscripten LIBRETRO="${LIBRETRO_NAME}" TARGET="${RA_TARGET}" clean ${RA_MAKE_ARGS}
  fi
  emmake make -f Makefile.emscripten LIBRETRO="${LIBRETRO_NAME}" TARGET="${RA_TARGET}" ${RA_MAKE_ARGS} ${RA_JOBS_ARG} all
  popd >/dev/null

  cp "${RETROARCH_DIR}/${RA_TARGET}" "${OUTPUT_DIR}/${RA_TARGET}"
  cp "${RETROARCH_DIR}/${ra_target_base}.wasm" "${OUTPUT_DIR}/${ra_target_base}.wasm"
  if [[ "${ENABLE_PTHREADS}" == "1" && -f "${RETROARCH_DIR}/${ra_target_base}.worker.js" ]]; then
    cp "${RETROARCH_DIR}/${ra_target_base}.worker.js" "${OUTPUT_DIR}/${ra_target_base}.worker.js"
  fi

  echo "Built RetroArch JS: ${OUTPUT_DIR}/${RA_TARGET}"
  echo "Built RetroArch WASM: ${OUTPUT_DIR}/${ra_target_base}.wasm"
  if [[ "${ENABLE_PTHREADS}" == "1" && -f "${OUTPUT_DIR}/${ra_target_base}.worker.js" ]]; then
    echo "Built RetroArch worker JS: ${OUTPUT_DIR}/${ra_target_base}.worker.js"
  fi
fi
