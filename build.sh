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
ENABLE_PTHREADS="${ENABLE_PTHREADS:-0}"
PTHREAD_POOL_SIZE="${PTHREAD_POOL_SIZE:-4}"
RA_JOBS="${RA_JOBS:-}"
RA_CLEAN="${RA_CLEAN:-1}"
RETROARCH_REPO="${RETROARCH_REPO:-https://github.com/libretro/RetroArch}"
RETROARCH_REF="${RETROARCH_REF:-}"
STATIC_LINKING="${STATIC_LINKING:-}"

RA_JOBS_ARG=""
if [[ -n "${RA_JOBS}" ]]; then
  RA_JOBS_ARG="-j${RA_JOBS}"
fi

if [[ "${ENABLE_PTHREADS}" == "1" ]]; then
  MAKE_ARGS="${MAKE_ARGS} CFLAGS+=-pthread CXXFLAGS+=-pthread"
  EMCC_ARGS="${EMCC_ARGS} -pthread"
  RA_MAKE_ARGS="${RA_MAKE_ARGS} HAVE_THREADS=1 PTHREAD_POOL_SIZE=${PTHREAD_POOL_SIZE}"
fi

if [[ -z "${STATIC_LINKING}" ]]; then
  if [[ "${BUILD_RETROARCH}" != "0" ]]; then
    STATIC_LINKING=1
  else
    STATIC_LINKING=0
  fi
fi

pushd "${ROOT_DIR}" >/dev/null

emmake make -f Makefile platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib clean STATIC_LINKING="${STATIC_LINKING}" ${MAKE_ARGS}
emmake make -f Makefile platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib STATIC_LINKING="${STATIC_LINKING}" ${MAKE_ARGS}

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

  echo "Built RetroArch JS: ${OUTPUT_DIR}/${LIBRETRO_NAME}.js"
  echo "Built RetroArch WASM: ${OUTPUT_DIR}/${LIBRETRO_NAME}.wasm"
fi
