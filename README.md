# Building with `build_slider.sh` (legacy)

```shell
$ build/build_slider.sh
```

## Disable LTO

**Note**: This only works on `raviole-mainline` branch.

```shell
$ LTO=none build/build_slider.sh
```

# Building with Bazel (recommended)

```shell
# set the distribution directory
$ DIST_DIR=out/dist
$ tools/bazel run //gs/kernel/device-modules:slider_dist -- --dist_dir=$DIST_DIR
```

## Disable LTO

**Note**: This only works on `raviole-mainline` branch.

```shell
# set the distribution directory
$ DIST_DIR=out/dist
$ tools/bazel run --lto=none //gs/kernel/device-modules:slider_dist -- --dist_dir=$DIST_DIR
```
