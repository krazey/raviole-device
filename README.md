# Building with Bazel (recommended)

```shell
# Files are copied to out/{branch}/dist
$ tools/bazel run //gs/google-modules/soc-modules:slider_dist
```

See `build/kernel/kleaf/README.md` for details.

## Disable LTO

**Note**: This only works on `raviole-mainline` branch.

```shell
# Files are copied to out/{branch}/dist
$ tools/bazel run --lto=none //gs/google-modules/soc-modules:slider_dist
```

# ABI monitoring with Bazel (recommended)

```shell
# Update symbol list common/android/abi_gki_aarch64_pixel
$ tools/bazel run //gs/google-modules/soc-modules:slider_abi_update_symbol_list

# Compare ABI and build files for distribution
$ tools/bazel build //gs/google-modules/soc-modules:slider_abi

# Update ABI common/android/abi_gki_aarch64.xml
$ tools/bazel run //gs/google-modules/soc-modules:slider_abi_update

# Copy files to distribution
$ tools/bazel run //gs/google-modules/soc-modules:slider_abi_dist
```

# Building with `build_slider.sh` (legacy)

```shell
$ build/build_slider.sh
```

## Disable LTO

**Note**: This only works on `raviole-mainline` branch.

```shell
$ LTO=none build/build_slider.sh
```
