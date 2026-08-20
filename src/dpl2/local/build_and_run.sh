#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
build_dir="${DPL2_OPENROAD_BUILD_DIR:-${repo_root}/build-dpl2-local}"
deps_prefix="${DPL2_OPENROAD_DEPS_PREFIX:-/tmp/openroad-dpl2-prefix}"

cmake_args=(
  -DBUILD_DPL2_LOCAL_TEST=ON
  -DBUILD_PYTHON=OFF
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

# This repository revision predates current Homebrew fmt/spdlog releases. The
# dependency prefix is optional, but when present it must contain the versions
# selected by OpenROAD's DependencyInstaller (fmt 8.1.1 and spdlog 1.9.2).
if [[ -d "${deps_prefix}" ]]; then
  cmake_args+=(
    "-DCMAKE_PREFIX_PATH=${deps_prefix}"
    "-Dfmt_DIR=${deps_prefix}/lib/cmake/fmt"
    "-Dspdlog_DIR=${deps_prefix}/lib/cmake/spdlog"
  )
  if [[ -x "${deps_prefix}/bin/swig" ]]; then
    cmake_args+=(
      "-DSWIG_EXECUTABLE=${deps_prefix}/bin/swig"
      "-DSWIG_DIR=${deps_prefix}/share/swig/4.1.0"
    )
  fi
fi

if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
  tcl_prefix="$(brew --prefix tcl-tk@8)"
  libomp_prefix="$(brew --prefix libomp)"
  boost_prefix="$(brew --prefix boost@1.85)"
  cmake_args+=(
    "-DTCL_LIBRARY=${tcl_prefix}/lib/libtcl8.6.dylib"
    "-DTCL_HEADER=${tcl_prefix}/include/tcl-tk"
    "-DOpenMP_CXX_FLAGS=-Xpreprocessor -fopenmp"
    -DOpenMP_CXX_LIB_NAMES=omp
    "-DOpenMP_CXX_INCLUDE_DIR=${libomp_prefix}/include"
    "-DOpenMP_omp_LIBRARY=${libomp_prefix}/lib/libomp.dylib"
    "-DBoost_ROOT=${boost_prefix}"
    "-DBoost_DIR=${boost_prefix}/lib/cmake/Boost-1.85.0"
    "-DBISON_EXECUTABLE=$(brew --prefix bison)/bin/bison"
    "-DFLEX_EXECUTABLE=$(brew --prefix flex)/bin/flex"
  )
fi

cmake -S "${repo_root}" -B "${build_dir}" "${cmake_args[@]}"
cmake --build "${build_dir}" --target openroad -j "${DPL2_BUILD_JOBS:-4}"

test_log="${build_dir}/dpl2-real-odb-test.log"
(
  cd "${repo_root}/src/dpl2/local/testdata"
  "${build_dir}/src/openroad" run_test_filler_repair.tcl
) | tee "${test_log}"
grep -q "proposals=" "${test_log}"
grep -q "invalidResults=0" "${test_log}"
grep -q "LEGAL: node=" "${test_log}"
grep -q "DPL2_REAL_ODB_TEST_PASS" "${test_log}"
