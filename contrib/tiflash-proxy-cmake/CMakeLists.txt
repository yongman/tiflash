# Copyright 2022 PingCAP, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

include (CheckCCompilerFlag)
include (CheckCXXCompilerFlag)
include (CheckCXXSourceRuns)
include (CMakePushCheckState)

option(
    TIFLASH_AUTO_DETECT_RUST_TARGET_FEATURES
    "Auto-detect host CPU features and pass matching CPU-specific codegen flags to internal Rust FFI builds."
    ${ENABLE_NEXT_GEN_COLUMNAR}
)

function(_tiflash_proxy_detect_host_x86_feature feature output_var)
    if (NOT ARCH_AMD64 OR CMAKE_CROSSCOMPILING)
        set (${output_var} FALSE PARENT_SCOPE)
        return()
    endif ()

    cmake_push_check_state (RESET)
    set (CMAKE_REQUIRED_QUIET TRUE)
    check_cxx_source_runs(
        "
        #if !defined(__GNUC__) && !defined(__clang__)
        #error unsupported compiler
        #endif

        int main() {
            __builtin_cpu_init();
            return __builtin_cpu_supports(\"${feature}\") ? 0 : 1;
        }
        "
        ${output_var}
    )
    cmake_pop_check_state ()
    set (${output_var} ${${output_var}} PARENT_SCOPE)
endfunction()

function(_tiflash_proxy_append_compiler_flag flag output_var)
    string (MAKE_C_IDENTIFIER "${flag}" flag_id)
    set (c_flag_var "TIFLASH_PROXY_C_FLAG_${flag_id}")
    set (cxx_flag_var "TIFLASH_PROXY_CXX_FLAG_${flag_id}")

    check_c_compiler_flag ("${flag}" ${c_flag_var})
    check_cxx_compiler_flag ("${flag}" ${cxx_flag_var})
    if (${c_flag_var} AND ${cxx_flag_var})
        set (flags ${${output_var}})
        list (APPEND flags "${flag}")
        set (${output_var} "${flags}" PARENT_SCOPE)
    endif ()
endfunction()

function(_tiflash_proxy_escape_toml_string input output_var)
    set (value "${input}")
    string (REPLACE "\\" "\\\\" value "${value}")
    string (REPLACE "\"" "\\\"" value "${value}")
    set (${output_var} "${value}" PARENT_SCOPE)
endfunction()

function(_tiflash_proxy_render_toml_string_list output_var)
    set (rendered "")
    foreach (item IN LISTS ARGN)
        _tiflash_proxy_escape_toml_string("${item}" escaped)
        string (APPEND rendered "    \"${escaped}\",\n")
    endforeach ()
    if (rendered)
        string (REGEX REPLACE ",\n$" "\n" rendered "${rendered}")
    endif ()
    set (${output_var} "${rendered}" PARENT_SCOPE)
endfunction()

set (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU "")
set (_TIFLASH_PROXY_AUTO_C_FLAGS "")
if (TIFLASH_AUTO_DETECT_RUST_TARGET_FEATURES)
    if (CMAKE_CROSSCOMPILING)
        message (STATUS "Skip TiFlash Rust FFI host CPU feature auto-detection while cross-compiling")
    elseif (ARCH_AMD64)
        _tiflash_proxy_detect_host_x86_feature("avx2" TIFLASH_PROXY_HOST_HAS_AVX2)
        _tiflash_proxy_detect_host_x86_feature("fma" TIFLASH_PROXY_HOST_HAS_FMA)
        _tiflash_proxy_detect_host_x86_feature("bmi2" TIFLASH_PROXY_HOST_HAS_BMI2)
        _tiflash_proxy_detect_host_x86_feature("avx512f" TIFLASH_PROXY_HOST_HAS_AVX512F)
        _tiflash_proxy_detect_host_x86_feature("avx512bw" TIFLASH_PROXY_HOST_HAS_AVX512BW)
        _tiflash_proxy_detect_host_x86_feature("avx512vl" TIFLASH_PROXY_HOST_HAS_AVX512VL)
        _tiflash_proxy_detect_host_x86_feature("avx512vnni" TIFLASH_PROXY_HOST_HAS_AVX512VNNI)
        _tiflash_proxy_detect_host_x86_feature("avx512bf16" TIFLASH_PROXY_HOST_HAS_AVX512BF16)
        _tiflash_proxy_detect_host_x86_feature("avx512fp16" TIFLASH_PROXY_HOST_HAS_AVX512FP16)

        if (HAVE_AVX2 AND TIFLASH_ENABLE_AVX_SUPPORT AND TIFLASH_PROXY_HOST_HAS_AVX2 AND TIFLASH_PROXY_HOST_HAS_FMA)
            set (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU "haswell")
            _tiflash_proxy_append_compiler_flag("-mavx2" _TIFLASH_PROXY_AUTO_C_FLAGS)
            _tiflash_proxy_append_compiler_flag("-mfma" _TIFLASH_PROXY_AUTO_C_FLAGS)
            _tiflash_proxy_append_compiler_flag("-mf16c" _TIFLASH_PROXY_AUTO_C_FLAGS)

            if (TIFLASH_PROXY_HOST_HAS_BMI2)
                _tiflash_proxy_append_compiler_flag("-mbmi2" _TIFLASH_PROXY_AUTO_C_FLAGS)
            endif ()
        endif ()

        if (HAVE_AVX512 AND TIFLASH_ENABLE_AVX512_SUPPORT
            AND TIFLASH_PROXY_HOST_HAS_AVX512F
            AND TIFLASH_PROXY_HOST_HAS_AVX512BW
            AND TIFLASH_PROXY_HOST_HAS_AVX512VL)
            set (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU "skylake-avx512")
            _tiflash_proxy_append_compiler_flag("-mavx512f" _TIFLASH_PROXY_AUTO_C_FLAGS)
            _tiflash_proxy_append_compiler_flag("-mavx512bw" _TIFLASH_PROXY_AUTO_C_FLAGS)
            _tiflash_proxy_append_compiler_flag("-mavx512vl" _TIFLASH_PROXY_AUTO_C_FLAGS)

            if (TIFLASH_PROXY_HOST_HAS_AVX512VNNI)
                set (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU "icelake-server")
                _tiflash_proxy_append_compiler_flag("-mavx512vnni" _TIFLASH_PROXY_AUTO_C_FLAGS)
            endif ()

            if (TIFLASH_PROXY_HOST_HAS_AVX512BF16)
                set (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU "sapphirerapids")
                _tiflash_proxy_append_compiler_flag("-mavx512bf16" _TIFLASH_PROXY_AUTO_C_FLAGS)
            endif ()

            if (TIFLASH_PROXY_HOST_HAS_AVX512FP16)
                set (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU "sapphirerapids")
                _tiflash_proxy_append_compiler_flag("-mavx512fp16" _TIFLASH_PROXY_AUTO_C_FLAGS)
            endif ()
        endif ()
    endif ()
endif ()

list (REMOVE_DUPLICATES _TIFLASH_PROXY_AUTO_C_FLAGS)

set(_TIFLASH_PROXY_ENABLE_FEATURES "")
if (ENABLE_JEMALLOC)
    if (APPLE)
        message(STATUS "proxy's jemalloc is disabled (AppleOS)")
    else()
        message(STATUS "proxy's jemalloc is enabled")
        if (ENABLE_NEXT_GEN_COLUMNAR)
            set(_TIFLASH_PROXY_ENABLE_FEATURES ENABLE_FEATURES="external-jemalloc")
        elseif (ENABLE_NEXT_GEN)
            set(_TIFLASH_PROXY_ENABLE_FEATURES ENABLE_FEATURES="raftstore-proxy/external-jemalloc")
        else()
            set(_TIFLASH_PROXY_ENABLE_FEATURES ENABLE_FEATURES="external-jemalloc")
        endif()
    endif()
else()
    message(STATUS "proxy's jemalloc is disabled")
endif()
if (CMAKE_BUILD_TYPE_UC STREQUAL "DEBUG" OR SAN_DEBUG)
    set(_TIFLASH_PROXY_BUILD_PROFILE "debug")
else()
    set(_TIFLASH_PROXY_BUILD_PROFILE "release")
endif()
set(_TIFLASH_PROXY_MAKE_COMMAND "${_TIFLASH_PROXY_ENABLE_FEATURES}" make "${_TIFLASH_PROXY_BUILD_PROFILE}")

# use different tiflash-proxy for classic, next-gen, or columnar
if (ENABLE_NEXT_GEN_COLUMNAR)
    set(_TIFLASH_PROXY_SOURCE_DIR "${TiFlash_SOURCE_DIR}/contrib/tiflash-columnar-hub")
elseif (ENABLE_NEXT_GEN)
    set(_TIFLASH_PROXY_SOURCE_DIR "${TiFlash_SOURCE_DIR}/contrib/tiflash-proxy-next-gen")
else()
    set(_TIFLASH_PROXY_SOURCE_DIR "${TiFlash_SOURCE_DIR}/contrib/tiflash-proxy")
endif()
message(STATUS "Using TiFlash Rust FFI source dir: ${_TIFLASH_PROXY_SOURCE_DIR}, make command: ${_TIFLASH_PROXY_MAKE_COMMAND}")

set(_TIFLASH_PROXY_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(_TIFLASH_PROXY_LIBRARY_NAME "${CMAKE_SHARED_LIBRARY_PREFIX}tiflash_proxy${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(_TIFLASH_PROXY_LIBRARY_DIR "${_TIFLASH_PROXY_CARGO_TARGET_DIR}/${_TIFLASH_PROXY_BUILD_PROFILE}")
file(GLOB_RECURSE _TIFLASH_PROXY_SRCS "${_TIFLASH_PROXY_SOURCE_DIR}/*.rs")
list(FILTER _TIFLASH_PROXY_SRCS EXCLUDE REGEX "/target/")
set(_TIFLASH_PROXY_MAKEFILE "${_TIFLASH_PROXY_SOURCE_DIR}/Makefile")
set(_TIFLASH_PROXY_CARGO_MANIFEST "${_TIFLASH_PROXY_SOURCE_DIR}/Cargo.toml")
set(_TIFLASH_PROXY_CARGO_LOCK "${_TIFLASH_PROXY_SOURCE_DIR}/Cargo.lock")
set(_TIFLASH_PROXY_RUST_TOOLCHAIN "${TiFlash_SOURCE_DIR}/rust-toolchain.toml")
if (EXISTS "${_TIFLASH_PROXY_SOURCE_DIR}/rust-toolchain.toml")
    set(_TIFLASH_PROXY_RUST_TOOLCHAIN "${_TIFLASH_PROXY_SOURCE_DIR}/rust-toolchain.toml")
elseif (EXISTS "${_TIFLASH_PROXY_SOURCE_DIR}/rust-toolchain")
    set(_TIFLASH_PROXY_RUST_TOOLCHAIN "${_TIFLASH_PROXY_SOURCE_DIR}/rust-toolchain")
endif()

# Build in the build directory instead of the default source directory
set(TIFLASH_RUST_ENV "CARGO_TARGET_DIR=${_TIFLASH_PROXY_CARGO_TARGET_DIR}" ${TIFLASH_RUST_ENV})

# Set CMAKE_POLICY_VERSION_MINIMUM to support CMake 4.0+ with older crate CMakeLists.txt files.
set(TIFLASH_RUST_ENV "CMAKE_POLICY_VERSION_MINIMUM=3.5" ${TIFLASH_RUST_ENV})

# use `CFLAGS=-w CXXFLAGS=-w` to inhibit warning messages.
if (TIFLASH_LLVM_TOOLCHAIN)
    set(_TIFLASH_PROXY_BASE_C_FLAGS "-w -fuse-ld=lld")
    set(_TIFLASH_PROXY_BASE_CXX_FLAGS "-w -fuse-ld=lld -stdlib=libc++")
else()
    set(_TIFLASH_PROXY_BASE_C_FLAGS "-w")
    set(_TIFLASH_PROXY_BASE_CXX_FLAGS "-w")
endif()

if (_TIFLASH_PROXY_AUTO_C_FLAGS)
    list (JOIN _TIFLASH_PROXY_AUTO_C_FLAGS " " _TIFLASH_PROXY_CPU_FLAGS)
    string (APPEND _TIFLASH_PROXY_BASE_C_FLAGS " ${_TIFLASH_PROXY_CPU_FLAGS}")
    string (APPEND _TIFLASH_PROXY_BASE_CXX_FLAGS " ${_TIFLASH_PROXY_CPU_FLAGS}")
endif ()

set(TIFLASH_RUST_ENV
    CMAKE=${CMAKE_COMMAND}
    "CFLAGS=${_TIFLASH_PROXY_BASE_C_FLAGS}"
    "CXXFLAGS=${_TIFLASH_PROXY_BASE_CXX_FLAGS}"
    ${TIFLASH_RUST_ENV})

set(TIFLASH_RUST_ENV PROTOC=${Protobuf_PROTOC_EXECUTABLE} PROTOC_INCLUDE=${Protobuf_INCLUDE_DIR} ${TIFLASH_RUST_ENV})

set (TIFLASH_RUSTFLAGS "$ENV{RUSTFLAGS}")
set (_TIFLASH_PROXY_TARGET_RUSTFLAGS "")
set (_TIFLASH_PROXY_CARGO_CONFIG "")
if (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU)
    list (APPEND _TIFLASH_PROXY_TARGET_RUSTFLAGS "-Ctarget-cpu=${_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU}")
endif ()
if (ENABLE_NEXT_GEN_COLUMNAR AND NOT APPLE)
    list (APPEND _TIFLASH_PROXY_TARGET_RUSTFLAGS "-Clink-arg=-Wl,--allow-multiple-definition")
endif ()

if (TIFLASH_LLVM_TOOLCHAIN AND USE_LIBCXX)
    set (TIFLASH_RUST_LINKER ${CMAKE_CURRENT_BINARY_DIR}/tiflash-linker)
    set (TIFLASH_RUST_LINKER_TMP ${CMAKE_CURRENT_BINARY_DIR}/tmp/tiflash-linker)
    set (_TIFLASH_RUST_LINKER_EXTRA_ARGS "")
    if (ENABLE_NEXT_GEN_COLUMNAR AND NOT APPLE)
        set (_TIFLASH_RUST_LINKER_EXTRA_ARGS " -Wl,--allow-multiple-definition")
    endif ()
    file (WRITE ${TIFLASH_RUST_LINKER_TMP}
            "#!/usr/bin/env sh\n${CMAKE_CXX_COMPILER} -l:libc++.so -l:libc++abi.so${_TIFLASH_RUST_LINKER_EXTRA_ARGS} $@")
    file (COPY ${TIFLASH_RUST_LINKER_TMP}
            DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
            FILE_PERMISSIONS OWNER_WRITE OWNER_READ OWNER_EXECUTE)
    if (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU)
        set (_TIFLASH_PROXY_TARGET_LINKER ${TIFLASH_RUST_LINKER})
        list (APPEND _TIFLASH_PROXY_TARGET_RUSTFLAGS "-Crpath=yes")
        if (ARCH_AARCH64)
            list (APPEND _TIFLASH_PROXY_TARGET_RUSTFLAGS "-Clink-arg=-Wl,-Bsymbolic")
        endif ()
        if (LINKER_NAME)
            list (APPEND _TIFLASH_PROXY_TARGET_RUSTFLAGS "-Clink-arg=-fuse-ld=${LINKER_NAME}")
        endif ()
    else ()
        string (APPEND TIFLASH_RUSTFLAGS " -C rpath=yes -C linker=${TIFLASH_RUST_LINKER}")
        if (ARCH_AARCH64)
            string (APPEND TIFLASH_RUSTFLAGS " -C link-arg=-Wl,-Bsymbolic")
        endif ()
        if (LINKER_NAME)
            string (APPEND TIFLASH_RUSTFLAGS " -C link-arg=-fuse-ld=${LINKER_NAME}")
        endif ()
    endif ()
    set (TIFLASH_RUST_ENV CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} CXXSTDLIB=c++ ${TIFLASH_RUST_ENV})
    message (STATUS "Enforce LLVM toolchain for rust")
endif ()

if (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU)
    execute_process(
        COMMAND rustc -vV
        WORKING_DIRECTORY ${_TIFLASH_PROXY_SOURCE_DIR}
        OUTPUT_VARIABLE _TIFLASH_PROXY_RUSTC_VERSION_VERBOSE
        RESULT_VARIABLE _TIFLASH_PROXY_RUSTC_VERSION_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (NOT _TIFLASH_PROXY_RUSTC_VERSION_RESULT EQUAL 0)
        message (FATAL_ERROR "Failed to determine rust host triple for TiFlash Rust FFI build")
    endif ()
    string (REGEX MATCH "host: ([^\n\r]+)" _TIFLASH_PROXY_RUST_HOST_TRIPLE_MATCH "${_TIFLASH_PROXY_RUSTC_VERSION_VERBOSE}")
    if (NOT CMAKE_MATCH_1)
        message (FATAL_ERROR "Failed to parse rust host triple from rustc -vV output")
    endif ()
    set (_TIFLASH_PROXY_RUST_HOST_TRIPLE "${CMAKE_MATCH_1}")

    set (_TIFLASH_PROXY_CARGO_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/tiflash-cargo-target-config.toml")
    set (_TIFLASH_PROXY_CARGO_CONFIG_CONTENT "target-applies-to-host = false\n\n")
    string (APPEND _TIFLASH_PROXY_CARGO_CONFIG_CONTENT "[target.${_TIFLASH_PROXY_RUST_HOST_TRIPLE}]\n")
    if (_TIFLASH_PROXY_TARGET_LINKER)
        _tiflash_proxy_escape_toml_string("${_TIFLASH_PROXY_TARGET_LINKER}" _TIFLASH_PROXY_TARGET_LINKER_ESCAPED)
        string (APPEND _TIFLASH_PROXY_CARGO_CONFIG_CONTENT "linker = \"${_TIFLASH_PROXY_TARGET_LINKER_ESCAPED}\"\n")
    endif ()
    if (_TIFLASH_PROXY_TARGET_RUSTFLAGS)
        _tiflash_proxy_render_toml_string_list(_TIFLASH_PROXY_TARGET_RUSTFLAGS_TOML ${_TIFLASH_PROXY_TARGET_RUSTFLAGS})
        string (APPEND _TIFLASH_PROXY_CARGO_CONFIG_CONTENT "rustflags = [\n${_TIFLASH_PROXY_TARGET_RUSTFLAGS_TOML}]\n")
    endif ()
    file (WRITE ${_TIFLASH_PROXY_CARGO_CONFIG} "${_TIFLASH_PROXY_CARGO_CONFIG_CONTENT}")

    set (TIFLASH_RUST_ENV
        "RUSTFLAGS="
        TIFLASH_USE_CARGO_HOST_TARGET_CONFIG=1
        "CARGO_GLOBAL_ARGS=-Zhost-config -Ztarget-applies-to-host --config ${_TIFLASH_PROXY_CARGO_CONFIG}"
        "CARGO_BUILD_ARGS=--target ${_TIFLASH_PROXY_RUST_HOST_TRIPLE}"
        ${TIFLASH_RUST_ENV})
    set (_TIFLASH_PROXY_LIBRARY_DIR
        "${_TIFLASH_PROXY_CARGO_TARGET_DIR}/${_TIFLASH_PROXY_RUST_HOST_TRIPLE}/${_TIFLASH_PROXY_BUILD_PROFILE}")
else ()
    string (STRIP "${TIFLASH_RUSTFLAGS}" TIFLASH_RUSTFLAGS)
    if (TIFLASH_RUSTFLAGS)
        set (TIFLASH_RUST_ENV "RUSTFLAGS=${TIFLASH_RUSTFLAGS}" ${TIFLASH_RUST_ENV})
    endif ()
endif ()
set (_TIFLASH_PROXY_LIBRARY "${_TIFLASH_PROXY_LIBRARY_DIR}/${_TIFLASH_PROXY_LIBRARY_NAME}")

if (_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU)
    message (STATUS "TiFlash Rust FFI host CPU target-cpu: ${_TIFLASH_PROXY_AUTO_RUST_TARGET_CPU}")
endif ()
if (_TIFLASH_PROXY_AUTO_C_FLAGS)
    message (STATUS "TiFlash Rust FFI propagated C/C++ CPU flags: ${_TIFLASH_PROXY_AUTO_C_FLAGS}")
endif ()

message(STATUS "Using rust env for tiflash-proxy: ${TIFLASH_RUST_ENV}")

set(_TIFLASH_PROXY_CUSTOM_DEPENDS
    "${_TIFLASH_PROXY_SRCS}"
    "${_TIFLASH_PROXY_MAKEFILE}"
    "${_TIFLASH_PROXY_CARGO_MANIFEST}"
    "${_TIFLASH_PROXY_RUST_TOOLCHAIN}")
if (EXISTS "${_TIFLASH_PROXY_CARGO_LOCK}")
    list(APPEND _TIFLASH_PROXY_CUSTOM_DEPENDS "${_TIFLASH_PROXY_CARGO_LOCK}")
endif()
if (_TIFLASH_PROXY_CARGO_CONFIG)
    list(APPEND _TIFLASH_PROXY_CUSTOM_DEPENDS "${_TIFLASH_PROXY_CARGO_CONFIG}")
endif()

add_custom_command(OUTPUT ${_TIFLASH_PROXY_LIBRARY}
    COMMENT "Building TiFlash Proxy using ${_TIFLASH_PROXY_BUILD_PROFILE} profile"
    COMMAND ${CMAKE_COMMAND} -E env ${TIFLASH_RUST_ENV} ${_TIFLASH_PROXY_MAKE_COMMAND}
    VERBATIM
    USES_TERMINAL
    WORKING_DIRECTORY ${_TIFLASH_PROXY_SOURCE_DIR}
    DEPENDS ${_TIFLASH_PROXY_CUSTOM_DEPENDS})

add_custom_target(tiflash_proxy ALL DEPENDS ${_TIFLASH_PROXY_LIBRARY})
add_library(libtiflash_proxy SHARED IMPORTED GLOBAL)
set_target_properties(libtiflash_proxy PROPERTIES IMPORTED_LOCATION ${_TIFLASH_PROXY_LIBRARY} IMPORTED_NO_SONAME ON)
add_dependencies(libtiflash_proxy tiflash_proxy)
