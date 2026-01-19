#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# User-configurable build flags.
# Add items as "KEY=VALUE" (one per line).
# Common flags apply to both builds; *_PTHREADS only when ENABLE_PTHREADS=1;
# *_NOPTHREADS only when ENABLE_PTHREADS=0.
CORE_MAKE_FLAGS_COMMON=(
)
CORE_MAKE_FLAGS_PTHREADS=(
  "HAVE_THREADS=1"
  "CHEEVOS=1"
)
CORE_MAKE_FLAGS_NOPTHREADS=(
  "HAVE_THREADS=0"
  "CHEEVOS=1"
)

RETROARCH_MAKE_FLAGS_COMMON=(
)
RETROARCH_MAKE_FLAGS_PTHREADS=(
  "HAVE_AL=0"
  "HAVE_CHEEVOS=1"
  "HAVE_THREADS=1"
  "PTHREAD_POOL_SIZE=1"
  "ALLOW_MEMORY_GROWTH=0"
  # Common sizes: 256MB=268435456, 512MB=536870912, 1GB=1073741824.
  # RetroArch default INITIAL_HEAP was 134217728 (128MB); MAXIMUM_MEMORY unset.
  "INITIAL_HEAP=536870912"
  "MAXIMUM_MEMORY=1073741824"
)
RETROARCH_MAKE_FLAGS_NOPTHREADS=(
  "HAVE_AL=0"
  "HAVE_CHEEVOS=1"
  "HAVE_THREADS=0"
  "PTHREAD_POOL_SIZE=0"
  # Common sizes: 256MB=268435456, 512MB=536870912, 1GB=1073741824.
  # RetroArch default INITIAL_HEAP was 134217728 (128MB); MAXIMUM_MEMORY unset.
  # RetroArch default ALLOW_MEMORY_GROWTH was 1 (enabled).
  "ALLOW_MEMORY_GROWTH=0"
  "INITIAL_HEAP=1073741824"
  "MAXIMUM_MEMORY=1073741824"
)

# Toolchain flags for the core build.
CORE_CFLAGS_COMMON=(
)
CORE_CFLAGS_PTHREADS=(
  "-pthread"
  "-s" "SHARED_MEMORY"
)
CORE_CFLAGS_NOPTHREADS=(
)

CORE_CXXFLAGS_COMMON=(
)
CORE_CXXFLAGS_PTHREADS=(
  "-pthread"
  "-s" "SHARED_MEMORY"
)
CORE_CXXFLAGS_NOPTHREADS=(
)

CORE_LDFLAGS_COMMON=(
)
CORE_LDFLAGS_PTHREADS=(
  "-pthread"
  "-s" "SHARED_MEMORY"
  "-s" "PTHREAD_POOL_SIZE=1"
)
CORE_LDFLAGS_NOPTHREADS=(
)

# emcc flags for the core JS build step.
EMCC_FLAGS_COMMON=(
)
EMCC_FLAGS_PTHREADS=(
  "-pthread"
  "-s" "SHARED_MEMORY"
  "-s" "PTHREAD_POOL_SIZE=1"
  "-s" "ALLOW_MEMORY_GROWTH=0"
  "-s" "MAXIMUM_MEMORY=536870912"
)
EMCC_FLAGS_NOPTHREADS=(
  "-s" "ALLOW_MEMORY_GROWTH=1"
  "-s" "MAXIMUM_MEMORY=536870912"
)

join_by() {
  local IFS="$1"
  shift
  echo "$*"
}

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
RA_MAKE_ARGS="${RA_MAKE_ARGS:-}"
RA_TARGET="${RA_TARGET:-${LIBRETRO_NAME}.js}"
ENABLE_PTHREADS="${ENABLE_PTHREADS:-0}"
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

ENABLE_PTHREADS="$(normalize_bool "${ENABLE_PTHREADS}")"

if [[ "${ENABLE_PTHREADS}" == "1" ]]; then
  core_flags="$(join_by ' ' "${CORE_MAKE_FLAGS_COMMON[@]-}" "${CORE_MAKE_FLAGS_PTHREADS[@]-}")"
  ra_flags="$(join_by ' ' "${RETROARCH_MAKE_FLAGS_COMMON[@]-}" "${RETROARCH_MAKE_FLAGS_PTHREADS[@]-}")"
  core_cflags="$(join_by ' ' "${CORE_CFLAGS_COMMON[@]-}" "${CORE_CFLAGS_PTHREADS[@]-}")"
  core_cxxflags="$(join_by ' ' "${CORE_CXXFLAGS_COMMON[@]-}" "${CORE_CXXFLAGS_PTHREADS[@]-}")"
  core_ldflags="$(join_by ' ' "${CORE_LDFLAGS_COMMON[@]-}" "${CORE_LDFLAGS_PTHREADS[@]-}")"
  emcc_flags="$(join_by ' ' "${EMCC_FLAGS_COMMON[@]-}" "${EMCC_FLAGS_PTHREADS[@]-}")"
else
  core_flags="$(join_by ' ' "${CORE_MAKE_FLAGS_COMMON[@]-}" "${CORE_MAKE_FLAGS_NOPTHREADS[@]-}")"
  ra_flags="$(join_by ' ' "${RETROARCH_MAKE_FLAGS_COMMON[@]-}" "${RETROARCH_MAKE_FLAGS_NOPTHREADS[@]-}")"
  core_cflags="$(join_by ' ' "${CORE_CFLAGS_COMMON[@]-}" "${CORE_CFLAGS_NOPTHREADS[@]-}")"
  core_cxxflags="$(join_by ' ' "${CORE_CXXFLAGS_COMMON[@]-}" "${CORE_CXXFLAGS_NOPTHREADS[@]-}")"
  core_ldflags="$(join_by ' ' "${CORE_LDFLAGS_COMMON[@]-}" "${CORE_LDFLAGS_NOPTHREADS[@]-}")"
  emcc_flags="$(join_by ' ' "${EMCC_FLAGS_COMMON[@]-}" "${EMCC_FLAGS_NOPTHREADS[@]-}")"
fi

if [[ -n "${MAKE_ARGS}" ]]; then
  MAKE_ARGS="${core_flags:+${core_flags} }${MAKE_ARGS}"
else
  MAKE_ARGS="${core_flags}"
fi
if [[ -n "${RA_MAKE_ARGS}" ]]; then
  RA_MAKE_ARGS="${ra_flags:+${ra_flags} }${RA_MAKE_ARGS}"
else
  RA_MAKE_ARGS="${ra_flags}"
fi
if [[ -n "${EMCC_ARGS}" ]]; then
  EMCC_ARGS="${emcc_flags:+${emcc_flags} }${EMCC_ARGS}"
else
  EMCC_ARGS="${emcc_flags}"
fi

MAKE_ENV=()
merged_cflags="${CFLAGS:-}"
if [[ -n "${core_cflags}" ]]; then
  merged_cflags="${merged_cflags:+${merged_cflags} }${core_cflags}"
fi
merged_cxxflags="${CXXFLAGS:-}"
if [[ -n "${core_cxxflags}" ]]; then
  merged_cxxflags="${merged_cxxflags:+${merged_cxxflags} }${core_cxxflags}"
fi
merged_ldflags="${LDFLAGS:-}"
if [[ -n "${core_ldflags}" ]]; then
  merged_ldflags="${merged_ldflags:+${merged_ldflags} }${core_ldflags}"
fi
MAKE_ENV+=("CFLAGS=${merged_cflags}")
MAKE_ENV+=("CXXFLAGS=${merged_cxxflags}")
MAKE_ENV+=("LDFLAGS=${merged_ldflags}")

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
