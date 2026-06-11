package=openssl
$(package)_version=3.5.7
$(package)_download_path=https://github.com/openssl/openssl/releases/download/openssl-$($(package)_version)
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8

define $(package)_set_vars
  $(package)_config_env=AR="$($(package)_ar)" RANLIB="$($(package)_ranlib)" CC="$($(package)_cc)"
  $(package)_config_opts=--prefix=$(host_prefix) --libdir=lib no-shared no-tests no-module no-dso no-apps
  $(package)_config_opts += no-ssl3 no-ssl3-method no-weak-ssl-ciphers
  $(package)_config_opts_freebsd=BSD-generic64
  $(package)_config_opts_mingw32=mingw64
  $(package)_config_opts_x86_64_linux=linux-x86_64
  $(package)_config_opts_aarch64_linux=linux-aarch64
  $(package)_config_opts_x86_64_darwin=darwin64-x86_64-cc
  $(package)_config_opts_aarch64_darwin=darwin64-arm64-cc
endef

define $(package)_config_cmds
  ./Configure $($(package)_config_opts)
endef

define $(package)_build_cmds
  $(MAKE) build_libs
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install_dev
endef

define $(package)_postprocess_cmds
  rm -rf bin share ssl lib/*.so lib/*.so.* lib/*.dylib lib/*.la
endef
