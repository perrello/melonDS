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
LIBRETRO_NAME="${LIBRETRO_NAME:-melonds}"
RETROARCH_DIR="${RETROARCH_DIR:-${ROOT_DIR}/retroarch-linker}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/dist-wasm}"
RA_MAKE_ARGS="${RA_MAKE_ARGS:-HAVE_AL=0 HAVE_CHEEVOS=1}"
MAXIMUM_MEMORY="${MAXIMUM_MEMORY:-}"
ENABLE_PTHREADS="${ENABLE_PTHREADS:-0}"
PTHREAD_POOL_SIZE="${PTHREAD_POOL_SIZE:-4}"
ENABLE_ASYNCIFY="${ENABLE_ASYNCIFY:-0}"
ENABLE_WFC_PROXY="${ENABLE_WFC_PROXY:-1}"
ASYNCIFY_STACK_SIZE="${ASYNCIFY_STACK_SIZE:-8192}"
RA_JOBS="${RA_JOBS:-}"
RA_CLEAN="${RA_CLEAN:-1}"
RETROARCH_REPO="${RETROARCH_REPO:-https://github.com/libretro/RetroArch}"
RETROARCH_REF="${RETROARCH_REF:-}"
STATIC_LINKING="${STATIC_LINKING:-}"

RA_JOBS_ARG=""
if [[ -n "${RA_JOBS}" ]]; then
  RA_JOBS_ARG="-j${RA_JOBS}"
fi

strip_threads_flags() {
  local input="$1"
  input="$(printf '%s' " ${input} " | sed -E \
    -e 's/ -pthread / /g' \
    -e 's/ -s SHARED_MEMORY / /g' \
    -e 's/ -s PTHREAD_POOL_SIZE=[^ ]+ / /g' \
    -e 's/ -s USE_PTHREADS=[^ ]+ / /g')"
  # Trim leading/trailing whitespace.
  input="${input#"${input%%[![:space:]]*}"}"
  input="${input%"${input##*[![:space:]]}"}"
  printf '%s' "${input}"
}

MAKE_ENV=()
if [[ "${ENABLE_PTHREADS}" == "1" ]]; then
  pthreads_cflags="-pthread -s SHARED_MEMORY"
  pthreads_ldflags="-pthread -s SHARED_MEMORY -s PTHREAD_POOL_SIZE=${PTHREAD_POOL_SIZE}"
  MAKE_ARGS="${MAKE_ARGS} HAVE_THREADS=1"
  EMCC_ARGS="${EMCC_ARGS} ${pthreads_ldflags}"
  RA_MAKE_ARGS="${RA_MAKE_ARGS} HAVE_THREADS=1 PTHREAD_POOL_SIZE=${PTHREAD_POOL_SIZE}"
  MAKE_ENV+=("CFLAGS=${CFLAGS:+${CFLAGS} }${pthreads_cflags}")
  MAKE_ENV+=("CXXFLAGS=${CXXFLAGS:+${CXXFLAGS} }${pthreads_cflags}")
  MAKE_ENV+=("LDFLAGS=${LDFLAGS:+${LDFLAGS} }${pthreads_ldflags}")
else
  MAKE_ARGS="${MAKE_ARGS} HAVE_THREADS=0"
  RA_MAKE_ARGS="${RA_MAKE_ARGS} HAVE_THREADS=0"
  EMCC_ARGS="$(strip_threads_flags "${EMCC_ARGS}")"
  CFLAGS="$(strip_threads_flags "${CFLAGS:-}")"
  CXXFLAGS="$(strip_threads_flags "${CXXFLAGS:-}")"
  LDFLAGS="$(strip_threads_flags "${LDFLAGS:-}")"
  export CFLAGS CXXFLAGS LDFLAGS
fi

if [[ -z "${STATIC_LINKING}" ]]; then
  if [[ "${BUILD_RETROARCH}" != "0" ]]; then
    STATIC_LINKING=1
  else
    STATIC_LINKING=0
  fi
fi

if [[ "${ENABLE_WFC_PROXY}" == "1" ]]; then
  ENABLE_ASYNCIFY=1
  EMCC_ARGS="${EMCC_ARGS} -s PROXY_POSIX_SOCKETS=1 -lwebsocket.js"
  RA_MAKE_ARGS="${RA_MAKE_ARGS} WFC_PROXY=1"
fi

if [[ -n "${MAXIMUM_MEMORY}" ]]; then
  EMCC_ARGS="${EMCC_ARGS} -s MAXIMUM_MEMORY=${MAXIMUM_MEMORY}"
  RA_MAKE_ARGS="${RA_MAKE_ARGS} MAXIMUM_MEMORY=${MAXIMUM_MEMORY}"
fi

if [[ "${ENABLE_ASYNCIFY}" == "1" ]]; then
  if [[ "${ENABLE_WFC_PROXY}" == "1" ]]; then
    asyncify_flags="-s ASYNCIFY=1 -s ASYNCIFY_STACK_SIZE=${ASYNCIFY_STACK_SIZE}"
    EMCC_ARGS="${EMCC_ARGS} ${asyncify_flags}"
    RA_MAKE_ARGS="${RA_MAKE_ARGS} ASYNC=1 MIN_ASYNC=0"
  else
    asyncify_flags="-s ASYNCIFY=1 -s ASYNCIFY_STACK_SIZE=${ASYNCIFY_STACK_SIZE} -s ASYNCIFY_IGNORE_INDIRECT=1 -s ASYNCIFY_ADD=netpacket_bridge_wait"
    EMCC_ARGS="${EMCC_ARGS} ${asyncify_flags}"
    RA_MAKE_ARGS="${RA_MAKE_ARGS} MIN_ASYNC=1 ASYNCIFY_ADD=dynCall_*,emscripten_mainloop,netpacket_bridge_wait"
  fi
fi

pushd "${ROOT_DIR}" >/dev/null

make_clean_cmd=(emmake make -f Makefile platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib clean STATIC_LINKING="${STATIC_LINKING}" ${MAKE_ARGS})
make_build_cmd=(emmake make -f Makefile platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib STATIC_LINKING="${STATIC_LINKING}" ${MAKE_ARGS})
if [[ ${#MAKE_ENV[@]} -gt 0 ]]; then
  env "${MAKE_ENV[@]}" "${make_clean_cmd[@]}"
  env "${MAKE_ENV[@]}" "${make_build_cmd[@]}"
else
  "${make_clean_cmd[@]}"
  "${make_build_cmd[@]}"
fi

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

  mkdir -p "${OUTPUT_DIR}"
  cp "${BC_FILE}" "${RETROARCH_DIR}/libretro_emscripten.bc"
  cp "${BC_FILE}" "${RETROARCH_DIR}/libretro_emscripten.a"

  pushd "${RETROARCH_DIR}" >/dev/null
  if [[ "${RA_CLEAN}" != "0" ]]; then
    emmake make -f Makefile.emscripten LIBRETRO="${LIBRETRO_NAME}" clean ${RA_MAKE_ARGS}
  fi
  emmake make -f Makefile.emscripten LIBRETRO="${LIBRETRO_NAME}" ${RA_MAKE_ARGS} ${RA_JOBS_ARG} all
  popd >/dev/null

  cp "${RETROARCH_DIR}/${LIBRETRO_NAME}_libretro.js" "${OUTPUT_DIR}/${LIBRETRO_NAME}.js"
  cp "${RETROARCH_DIR}/${LIBRETRO_NAME}_libretro.wasm" "${OUTPUT_DIR}/${LIBRETRO_NAME}.wasm"
  if [[ "${ENABLE_PTHREADS}" != "1" ]]; then
    rm -f "${RETROARCH_DIR}/${LIBRETRO_NAME}_libretro.worker.js"
    rm -f "${OUTPUT_DIR}/${LIBRETRO_NAME}_libretro.worker.js"
  fi
  if [[ "${ENABLE_PTHREADS}" == "1" && -f "${RETROARCH_DIR}/${LIBRETRO_NAME}_libretro.worker.js" ]]; then
    cp "${RETROARCH_DIR}/${LIBRETRO_NAME}_libretro.worker.js" "${OUTPUT_DIR}/${LIBRETRO_NAME}_libretro.worker.js"
  fi

  echo "Built RetroArch JS: ${OUTPUT_DIR}/${LIBRETRO_NAME}.js"
  echo "Built RetroArch WASM: ${OUTPUT_DIR}/${LIBRETRO_NAME}.wasm"
  if [[ -f "${OUTPUT_DIR}/${LIBRETRO_NAME}_libretro.worker.js" ]]; then
    echo "Built RetroArch worker JS: ${OUTPUT_DIR}/${LIBRETRO_NAME}_libretro.worker.js"
  fi
fi
