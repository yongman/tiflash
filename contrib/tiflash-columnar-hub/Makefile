SHELL := bash
ENABLE_FEATURES ?=
PROJECT_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
UNAME_S := $(shell uname -s)
CARGO_GLOBAL_ARGS ?=
CARGO_BUILD_ARGS ?=

# Build-time environment, captured for reporting by the application binary.
BUILD_INFO_GIT_FALLBACK := "Unknown (no git or not git repo)"
BUILD_INFO_RUSTC_FALLBACK := "Unknown"
export PROXY_BUILD_TIME := ""
export PROXY_BUILD_RUSTC_VERSION := $(shell rustc --version 2> /dev/null || echo ${BUILD_INFO_RUSTC_FALLBACK})
export PROXY_BUILD_GIT_HASH ?= $(shell git -C $(PROJECT_DIR) rev-parse HEAD 2> /dev/null || echo ${BUILD_INFO_GIT_FALLBACK})
export PROXY_BUILD_GIT_BRANCH ?= $(shell git -C $(PROJECT_DIR) rev-parse --abbrev-ref HEAD 2> /dev/null || echo ${BUILD_INFO_GIT_FALLBACK})
export PROXY_PROFILE ?= debug

# `kvengine` pulls in both `usearch` and the standalone `simsimd` crate from
# cloud-storage-engine. They currently export the same C symbols, so allow the
# linker to coalesce them in the columnar hub build instead of vendoring
# another patched `usearch` copy locally.
ifneq ($(UNAME_S),Darwin)
ifeq ($(strip $(TIFLASH_USE_CARGO_HOST_TARGET_CONFIG)),)
export RUSTFLAGS := $(RUSTFLAGS) -C link-arg=-Wl,--allow-multiple-definition
endif
endif

FEATURE_ARGS :=
ifneq ($(strip $(ENABLE_FEATURES)),)
FEATURE_ARGS += --features $(ENABLE_FEATURES)
endif

.PHONY: debug release build clean

build:
	cargo $(CARGO_GLOBAL_ARGS) build --locked --manifest-path hub-runtime/Cargo.toml $(PROFILE_FLAG) $(CARGO_BUILD_ARGS) $(FEATURE_ARGS)

debug: export PROXY_PROFILE=debug
debug:
	cargo $(CARGO_GLOBAL_ARGS) build --locked --manifest-path hub-runtime/Cargo.toml $(CARGO_BUILD_ARGS) $(FEATURE_ARGS)

release: export PROXY_PROFILE=release
release:
	cargo $(CARGO_GLOBAL_ARGS) build --locked --manifest-path hub-runtime/Cargo.toml --release $(CARGO_BUILD_ARGS) $(FEATURE_ARGS)

clean:
	cargo $(CARGO_GLOBAL_ARGS) clean --manifest-path hub-runtime/Cargo.toml $(CARGO_BUILD_ARGS)
