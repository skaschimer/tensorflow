"""TensorFlow workspace initialization. Consult the WORKSPACE on how to use it."""

load("@bazel_features//:deps.bzl", "bazel_features_deps")
load("@bazel_skylib//lib:versions.bzl", "versions")
load("@bazel_tools//tools/build_defs/repo:java.bzl", "java_import_external")
load("@io_bazel_rules_closure//closure:defs.bzl", "filegroup_external")
load("//third_party:repo.bzl", "tf_http_archive", "tf_mirror_urls")
load("//third_party/absl:workspace.bzl", absl = "repo")
load("//third_party/benchmark:workspace.bzl", benchmark = "repo")
load("//third_party/clang_toolchain:cc_configure_clang.bzl", "cc_download_clang_toolchain")
load("//third_party/dlpack:workspace.bzl", dlpack = "repo")
load("//third_party/ducc:workspace.bzl", ducc = "repo")
load("//third_party/eigen3:workspace.bzl", eigen3 = "repo")
load("//third_party/farmhash:workspace.bzl", farmhash = "repo")
load("//third_party/FP16:workspace.bzl", FP16 = "repo")
load("//third_party/gemmlowp:workspace.bzl", gemmlowp = "repo")
load("//third_party/git:git_configure.bzl", "git_configure")
load("//third_party/gloo:workspace.bzl", gloo = "repo")
load("//third_party/gpus:rocm_configure.bzl", "rocm_configure")
load("//third_party/gpus:sycl_configure.bzl", "sycl_configure")
load("//third_party/highwayhash:workspace.bzl", highwayhash = "repo")
load("//third_party/hwloc:workspace.bzl", hwloc = "repo")
load("//third_party/implib_so:workspace.bzl", implib_so = "repo")
load("//third_party/llvm:workspace.bzl", llvm = "repo")
load("//third_party/mpitrampoline:workspace.bzl", mpitrampoline = "repo")
load("//third_party/nanobind:workspace.bzl", nanobind = "repo")
load("//third_party/nasm:workspace.bzl", nasm = "repo")
load("//third_party/nvshmem:workspace.bzl", nvshmem = "repo")
load("//third_party/py:python_configure.bzl", "python_configure")
load("//third_party/py/ml_dtypes:workspace.bzl", ml_dtypes = "repo")
load("//third_party/pybind11_abseil:workspace.bzl", pybind11_abseil = "repo")
load("//third_party/pybind11_bazel:workspace.bzl", pybind11_bazel = "repo")
load("//third_party/robin_map:workspace.bzl", robin_map = "repo")
load("//third_party/shardy:workspace.bzl", shardy = "repo")
load("//third_party/stablehlo:workspace.bzl", stablehlo = "repo")
load("//third_party/tensorrt:tensorrt_configure.bzl", "tensorrt_configure")
load("//third_party/tensorrt:workspace.bzl", tensorrt = "repo")
load("//third_party/triton:workspace.bzl", triton = "repo")
load("//third_party/uv:workspace.bzl", uv = "repo")
load("//tools/def_file_filter:def_file_filter_configure.bzl", "def_file_filter_configure")
load("//tools/toolchains:cpus/aarch64/aarch64_compiler_configure.bzl", "aarch64_compiler_configure")
load("//tools/toolchains:cpus/arm/arm_compiler_configure.bzl", "arm_compiler_configure")
load("//tools/toolchains/clang6:repo.bzl", "clang6_configure")
load("//tools/toolchains/embedded/arm-linux:arm_linux_toolchain_configure.bzl", "arm_linux_toolchain_configure")
load("//tools/toolchains/remote:configure.bzl", "remote_execution_configure")
load("//tools/toolchains/remote_config:configs.bzl", "initialize_rbe_configs")

def _initialize_third_party():
    """ Load third party repositories.  See above load() statements. """
    FP16()
    absl()
    benchmark()
    dlpack()
    ducc()
    eigen3()
    farmhash()
    gemmlowp()
    gloo()
    highwayhash()
    hwloc()
    implib_so()
    ml_dtypes()
    mpitrampoline()
    nanobind()
    nasm()
    nvshmem()
    pybind11_abseil()
    pybind11_bazel()
    robin_map()
    shardy()
    stablehlo()
    tensorrt()
    triton()
    uv()

    # copybara: tsl vendor

# Toolchains & platforms required by Tensorflow to build.
def _tf_toolchains():
    native.register_execution_platforms("@local_execution_config_platform//:platform")
    native.register_toolchains("@local_execution_config_python//:py_toolchain")

    # Loads all external repos to configure RBE builds.
    initialize_rbe_configs()

    # Note that we check the minimum bazel version in WORKSPACE.
    clang6_configure(name = "local_config_clang6")
    cc_download_clang_toolchain(name = "local_config_download_clang")
    tensorrt_configure(name = "local_config_tensorrt")
    git_configure(name = "local_config_git")
    python_configure(name = "local_config_python")
    rocm_configure(name = "local_config_rocm")
    sycl_configure(name = "local_config_sycl")
    remote_execution_configure(name = "local_config_remote_execution")

    # For windows bazel build
    # TODO: Remove def file filter when TensorFlow can export symbols properly on Windows.
    def_file_filter_configure(name = "local_config_def_file_filter")

    # Point //external/local_config_arm_compiler to //external/arm_compiler
    arm_compiler_configure(
        name = "local_config_arm_compiler",
        build_file = "//tools/toolchains/cpus/arm:template.BUILD",
        remote_config_repo_arm = "../arm_compiler",
        remote_config_repo_aarch64 = "../aarch64_compiler",
    )

    # Load aarch64 toolchain
    aarch64_compiler_configure()

    # TFLite crossbuild toolchain for embeddeds Linux
    arm_linux_toolchain_configure(
        name = "local_config_embedded_arm",
        build_file = "//tools/toolchains/embedded/arm-linux:template.BUILD",
        aarch64_repo = "../aarch64_linux_toolchain",
        armhf_repo = "../armhf_linux_toolchain",
    )

# Define all external repositories required by TensorFlow
def _tf_repositories():
    """All external dependencies for TF builds."""

    # To update any of the dependencies below:
    # a) update URL and strip_prefix to the new git commit hash
    # b) get the sha256 hash of the commit by running:
    #    curl -L <url> | sha256sum
    # and update the sha256 with the result.

    # LINT.IfChange
    tf_http_archive(
        name = "XNNPACK",
        sha256 = "44955ad1ef0ab8d2c658ae0ece05429729a17239c9b17ffcd16816212cd00e12",
        strip_prefix = "XNNPACK-585e73e63cb35c8a416c83a48ca9ab79f7f7d45e",
        urls = tf_mirror_urls("https://github.com/google/XNNPACK/archive/585e73e63cb35c8a416c83a48ca9ab79f7f7d45e.zip"),
    )
    # LINT.ThenChange(//tensorflow/lite/tools/cmake/modules/xnnpack.cmake)

    tf_http_archive(
        name = "KleidiAI",
        sha256 = "439926527fca9405ae90b602a3938d3435751ec78492e5f1c62d85f5df8c2784",
        strip_prefix = "kleidiai-dc69e899945c412a8ce39ccafd25139f743c60b1",
        urls = tf_mirror_urls("https://github.com/ARM-software/kleidiai/archive/dc69e899945c412a8ce39ccafd25139f743c60b1.zip"),
    )

    tf_http_archive(
        name = "FXdiv",
        sha256 = "3d7b0e9c4c658a84376a1086126be02f9b7f753caa95e009d9ac38d11da444db",
        strip_prefix = "FXdiv-63058eff77e11aa15bf531df5dd34395ec3017c8",
        urls = tf_mirror_urls("https://github.com/Maratyszcza/FXdiv/archive/63058eff77e11aa15bf531df5dd34395ec3017c8.zip"),
    )

    tf_http_archive(
        name = "cpuinfo",
        sha256 = "ae356c4c0c841e20711b5e111a1ccdec9c2f3c1dd7bde7cfba1bed18d6d02459",
        strip_prefix = "cpuinfo-de0ce7c7251372892e53ce9bc891750d2c9a4fd8",
        urls = tf_mirror_urls("https://github.com/pytorch/cpuinfo/archive/de0ce7c7251372892e53ce9bc891750d2c9a4fd8.zip"),
    )

    tf_http_archive(
        name = "pthreadpool",
        sha256 = "516ba8d05c30e016d7fd7af6a7fc74308273883f857faf92bc9bb630ab6dba2c",
        strip_prefix = "pthreadpool-c2ba5c50bb58d1397b693740cf75fad836a0d1bf",
        urls = tf_mirror_urls("https://github.com/google/pthreadpool/archive/c2ba5c50bb58d1397b693740cf75fad836a0d1bf.zip"),
    )

    tf_http_archive(
        name = "mkl_dnn_v1",
        build_file = "//third_party/mkl_dnn:mkldnn_v1.BUILD",
        sha256 = "dc2b9bc851cd8d5a6c4622f7dc215bdb6b32349962875f8bf55cceed45a4c449",
        strip_prefix = "oneDNN-2.7.1",
        urls = tf_mirror_urls("https://github.com/oneapi-src/oneDNN/archive/refs/tags/v2.7.1.tar.gz"),
    )

    tf_http_archive(
        name = "onednn",
        build_file = "//third_party/mkl_dnn:mkldnn_v1.BUILD",
        patch_file = ["//third_party/mkl_dnn:setting_init.patch"],
        sha256 = "071f289dc961b43a3b7c8cbe8a305290a7c5d308ec4b2f586397749abdc88296",
        strip_prefix = "oneDNN-3.7.3",
        urls = tf_mirror_urls("https://github.com/oneapi-src/oneDNN/archive/refs/tags/v3.7.3.tar.gz"),
    )

    tf_http_archive(
        name = "mkl_dnn_acl_compatible",
        build_file = "//third_party/mkl_dnn:mkldnn_acl.BUILD",
        patch_file = [
            "//third_party/mkl_dnn:onednn_acl_threadcap.patch",
            "//third_party/mkl_dnn:onednn_acl_reorder.patch",
            "//third_party/mkl_dnn:onednn_acl_thread_local_scheduler.patch",
            "//third_party/mkl_dnn:onednn_acl_fp32_bf16_reorder.patch",
            "//third_party/mkl_dnn:onednn_acl_bf16_capability_detection_for_ubuntu20.04.patch",
            "//third_party/mkl_dnn:onednn_acl_indirect_conv.patch",
            "//third_party/mkl_dnn:onednn_acl_allow_blocked_weight_format_for_matmul_primitive.patch",
            "//third_party/mkl_dnn:onednn_acl_fix_segfault_during_postop_execute.patch",
            "//third_party/mkl_dnn:onednn_acl_add_bf16_platform_support_check.patch",
            "//third_party/mkl_dnn:onednn_acl_add_sbgemm_matmul_primitive_definition.patch",
        ],
        sha256 = "2f76b407ef8893cca71340f88cd800019a1f14f8ac1bbdbb89a84be1370b52e3",
        strip_prefix = "oneDNN-3.2.1",
        urls = tf_mirror_urls("https://github.com/oneapi-src/oneDNN/archive/refs/tags/v3.2.1.tar.gz"),
    )

    tf_http_archive(
        name = "compute_library",
        patch_file = [
            "//third_party/compute_library:compute_library.patch",
            "//third_party/compute_library:acl_thread_local_scheduler.patch",
            "//third_party/compute_library:exclude_omp_scheduler.patch",
            "//third_party/compute_library:include_string.patch",
        ],
        sha256 = "c4ca329a78da380163b2d86e91ba728349b6f0ee97d66e260a694ef37f0b0d93",
        strip_prefix = "ComputeLibrary-23.05.1",
        urls = tf_mirror_urls("https://github.com/ARM-software/ComputeLibrary/archive/v23.05.1.tar.gz"),
    )

    tf_http_archive(
        name = "arm_compiler",
        build_file = "//:arm_compiler.BUILD",
        sha256 = "b9e7d50ffd9996ed18900d041d362c99473b382c0ae049b2fce3290632d2656f",
        strip_prefix = "rpi-newer-crosstools-eb68350c5c8ec1663b7fe52c742ac4271e3217c5/x64-gcc-6.5.0/arm-rpi-linux-gnueabihf/",
        urls = tf_mirror_urls("https://github.com/rvagg/rpi-newer-crosstools/archive/eb68350c5c8ec1663b7fe52c742ac4271e3217c5.tar.gz"),
    )

    tf_http_archive(
        # This is the latest `aarch64-none-linux-gnu` compiler provided by ARM
        # See https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain/gnu-a/downloads
        # The archive contains GCC version 9.2.1
        name = "aarch64_compiler",
        build_file = "//:arm_compiler.BUILD",
        sha256 = "8dfe681531f0bd04fb9c53cf3c0a3368c616aa85d48938eebe2b516376e06a66",
        strip_prefix = "gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu",
        urls = tf_mirror_urls("https://developer.arm.com/-/media/Files/downloads/gnu-a/9.2-2019.12/binrel/gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu.tar.xz"),
    )

    tf_http_archive(
        name = "aarch64_linux_toolchain",
        build_file = "//tools/toolchains/embedded/arm-linux:aarch64-linux-toolchain.BUILD",
        sha256 = "50cdef6c5baddaa00f60502cc8b59cc11065306ae575ad2f51e412a9b2a90364",
        strip_prefix = "arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu",
        urls = tf_mirror_urls("https://developer.arm.com/-/media/Files/downloads/gnu/11.3.rel1/binrel/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu.tar.xz"),
    )

    tf_http_archive(
        name = "armhf_linux_toolchain",
        build_file = "//tools/toolchains/embedded/arm-linux:armhf-linux-toolchain.BUILD",
        sha256 = "3f76650b1d048036473b16b647b8fd005ffccd1a2869c10994967e0e49f26ac2",
        strip_prefix = "arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-gnueabihf",
        urls = tf_mirror_urls("https://developer.arm.com/-/media/Files/downloads/gnu/11.3.rel1/binrel/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-gnueabihf.tar.xz"),
    )

    tf_http_archive(
        name = "com_googlesource_code_re2",
        sha256 = "ef516fb84824a597c4d5d0d6d330daedb18363b5a99eda87d027e6bdd9cba299",
        strip_prefix = "re2-03da4fc0857c285e3a26782f6bc8931c4c950df4",
        urls = tf_mirror_urls("https://github.com/google/re2/archive/03da4fc0857c285e3a26782f6bc8931c4c950df4.tar.gz"),
    )

    tf_http_archive(
        name = "com_github_google_crc32c",
        sha256 = "6b3b1d861bb8307658b2407bc7a4c59e566855ef5368a60b35c893551e4788e9",
        build_file = "@com_github_googlecloudplatform_google_cloud_cpp//bazel:crc32c.BUILD",
        strip_prefix = "crc32c-1.0.6",
        urls = tf_mirror_urls("https://github.com/google/crc32c/archive/1.0.6.tar.gz"),
    )

    tf_http_archive(
        name = "com_github_googlecloudplatform_tensorflow_gcp_tools",
        sha256 = "5e9ebe17eaa2895eb7f77fefbf52deeda7c4b63f5a616916b823eb74f3a0c542",
        strip_prefix = "tensorflow-gcp-tools-2643d8caeba6ca2a6a0b46bb123953cb95b7e7d5",
        urls = tf_mirror_urls("https://github.com/GoogleCloudPlatform/tensorflow-gcp-tools/archive/2643d8caeba6ca2a6a0b46bb123953cb95b7e7d5.tar.gz"),
    )

    tf_http_archive(
        name = "six_archive",
        build_file = "//third_party:six.BUILD",
        sha256 = "1e61c37477a1626458e36f7b1d82aa5c9b094fa4802892072e49de9c60c4c926",
        strip_prefix = "six-1.16.0",
        urls = tf_mirror_urls("https://pypi.python.org/packages/source/s/six/six-1.16.0.tar.gz"),
    )

    filegroup_external(
        name = "astunparse_license",
        licenses = ["notice"],  # PSFL
        sha256_urls = {
            "92fc0e4f4fa9460558eedf3412b988d433a2dcbb3a9c45402a145a4fab8a6ac6": tf_mirror_urls("https://raw.githubusercontent.com/simonpercivall/astunparse/v1.6.2/LICENSE"),
        },
    )

    tf_http_archive(
        name = "absl_py",
        sha256 = "8a3d0830e4eb4f66c4fa907c06edf6ce1c719ced811a12e26d9d3162f8471758",
        strip_prefix = "abseil-py-2.1.0",
        urls = tf_mirror_urls("https://github.com/abseil/abseil-py/archive/refs/tags/v2.1.0.tar.gz"),
    )

    filegroup_external(
        name = "org_python_license",
        licenses = ["notice"],  # Python 2.0
        sha256_urls = {
            "e76cacdf0bdd265ff074ccca03671c33126f597f39d0ed97bc3e5673d9170cf6": tf_mirror_urls("https://docs.python.org/2.7/_sources/license.rst.txt"),
        },
    )

    tf_http_archive(
        name = "com_google_protobuf",
        patch_file = ["//third_party/protobuf:protobuf-6.31.1.patch"],
        sha256 = "6e09bbc950ba60c3a7b30280210cd285af8d7d8ed5e0a6ed101c72aff22e8d88",
        strip_prefix = "protobuf-6.31.1",
        urls = tf_mirror_urls("https://github.com/protocolbuffers/protobuf/archive/refs/tags/v6.31.1.zip"),
        repo_mapping = {
            "@abseil-cpp": "@com_google_absl",
            "@protobuf_pip_deps": "@pypi",
        },
    )

    tf_http_archive(
        name = "com_google_googletest",
        # Use the commit on 2025/6/09:
        # https://github.com/google/googletest/commit/28e9d1f26771c6517c3b4be10254887673c94018
        sha256 = "f253ca1a07262f8efde8328e4b2c68979e40ddfcfc001f70d1d5f612c7de2974",
        strip_prefix = "googletest-28e9d1f26771c6517c3b4be10254887673c94018",
        # Patch googletest to:
        #   - avoid dependencies on @fuchsia_sdk,
        #   - refer to re2 as @com_googlesource_code_re2,
        #   - refer to abseil as @com_google_absl.
        #
        # To update the patch, run:
        # $ cd ~
        # $ mkdir -p github
        # $ cd github
        # $ git clone https://github.com/google/googletest.git
        # $ cd googletest
        # $ git checkout 28e9d1f26771c6517c3b4be10254887673c94018
        # ... make local changes to googletest ...
        # $ git diff > <client-root>/third_party/tensorflow/third_party/googletest/googletest.patch
        #
        # The patch path is relative to third_party/xla.
        patch_file = ["//third_party/googletest:googletest.patch"],
        urls = tf_mirror_urls("https://github.com/google/googletest/archive/28e9d1f26771c6517c3b4be10254887673c940189.zip"),
    )

    tf_http_archive(
        name = "com_google_fuzztest",
        sha256 = "d922940bde8904937b9e13298f06b1d59388ab4a965122860358b00535438f63",
        strip_prefix = "fuzztest-e576caaece16bd1f8dcd196736743c36474f3c16",
        urls = tf_mirror_urls("https://github.com/google/fuzztest/archive/e576caaece16bd1f8dcd196736743c36474f3c16.zip"),
    )

    tf_http_archive(
        name = "com_github_gflags_gflags",
        sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
        strip_prefix = "gflags-2.2.2",
        urls = tf_mirror_urls("https://github.com/gflags/gflags/archive/v2.2.2.tar.gz"),
    )

    tf_http_archive(
        name = "curl",
        build_file = "//third_party:curl.BUILD",
        sha256 = "264537d90e58d2b09dddc50944baf3c38e7089151c8986715e2aaeaaf2b8118f",
        strip_prefix = "curl-8.11.0",
        urls = tf_mirror_urls("https://curl.se/download/curl-8.11.0.tar.gz"),
    )

    tf_http_archive(
        name = "com_github_grpc_grpc",
        sha256 = "dd6a2fa311ba8441bbefd2764c55b99136ff10f7ea42954be96006a2723d33fc",
        strip_prefix = "grpc-1.74.0",
        patch_file = ["//third_party/grpc:grpc.patch"],
        urls = tf_mirror_urls("https://github.com/grpc/grpc/archive/refs/tags/v1.74.0.tar.gz"),
    )

    # Load the raw llvm-project.  llvm does not have build rules set up by default,
    # but provides a script for setting up build rules via overlays.
    llvm("llvm-raw")

    # Intel openMP that is part of LLVM sources.
    tf_http_archive(
        name = "llvm_openmp",
        build_file = "//third_party/llvm_openmp:BUILD.bazel",
        patch_file = ["//third_party/llvm_openmp:openmp_switch_default_patch.patch"],
        sha256 = "d19f728c8e04fb1e94566c8d76aef50ec926cd2f95ef3bf1e0a5de4909b28b44",
        strip_prefix = "openmp-10.0.1.src",
        urls = tf_mirror_urls("https://github.com/llvm/llvm-project/releases/download/llvmorg-10.0.1/openmp-10.0.1.src.tar.xz"),
    )

    tf_http_archive(
        name = "jsoncpp_git",
        sha256 = "f409856e5920c18d0c2fb85276e24ee607d2a09b5e7d5f0a371368903c275da2",
        strip_prefix = "jsoncpp-1.9.5",
        urls = tf_mirror_urls("https://github.com/open-source-parsers/jsoncpp/archive/1.9.5.tar.gz"),
    )

    tf_http_archive(
        name = "zlib",
        build_file = "//third_party:zlib.BUILD",
        sha256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
        strip_prefix = "zlib-1.3.1",
        urls = tf_mirror_urls("https://zlib.net/zlib-1.3.1.tar.gz"),
    )

    tf_http_archive(
        name = "snappy",
        build_file = "//third_party:snappy.BUILD",
        sha256 = "2e458b7017cd58dcf1469ab315389e85e7f445bd035188f2983f81fb19ecfb29",
        strip_prefix = "snappy-984b191f0fefdeb17050b42a90b7625999c13b8d",
        urls = tf_mirror_urls("https://github.com/google/snappy/archive/984b191f0fefdeb17050b42a90b7625999c13b8d.tar.gz"),
    )

    tf_http_archive(
        name = "net_zstd",
        build_file = "//third_party:net_zstd.BUILD",
        sha256 = "7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4",
        strip_prefix = "zstd-1.5.7/lib",
        urls = tf_mirror_urls("https://github.com/facebook/zstd/archive/v1.5.7.zip"),  # 2025-05-20
    )

    tf_http_archive(
        name = "cudnn_frontend_archive",
        build_file = "//third_party:cudnn_frontend.BUILD",
        patch_file = ["//third_party:cudnn_frontend_header_fix.patch"],
        sha256 = "34dfe01057e43e799af207522aa0c863ad3177f8c1568b6e7a7e4ccf1cbff769",
        strip_prefix = "cudnn-frontend-1.11.0",
        urls = tf_mirror_urls("https://github.com/NVIDIA/cudnn-frontend/archive/refs/tags/v1.11.0.zip"),
    )

    tf_http_archive(
        name = "cutlass_archive",
        build_file = "//third_party:cutlass.BUILD",
        sha256 = "a7739ca3dc74e3a5cb57f93fc95224c5e2a3c2dff2c16bb09a5e459463604c08",
        strip_prefix = "cutlass-3.8.0",
        urls = tf_mirror_urls("https://github.com/NVIDIA/cutlass/archive/refs/tags/v3.8.0.zip"),
    )

    tf_http_archive(
        name = "nccl_archive",
        build_file = "//third_party:nccl/archive.BUILD",
        patch_file = ["//third_party/nccl:archive.patch"],
        sha256 = "0c5199ea56c70beb72a00eaf0d29887ac90243b47ebba048fba33ad86fcc2322",
        strip_prefix = "nccl-2.26.5-1",
        urls = tf_mirror_urls("https://github.com/nvidia/nccl/archive/v2.26.5-1.tar.gz"),
    )

    tf_http_archive(
        name = "nvtx_archive",
        build_file = "//third_party:nvtx/BUILD.bazel",
        sha256 = "5a581c3234c5a6b2fd94363e3fdd5a4f5d2a3d9c53c4b9442b0784e6cdfe722c",
        strip_prefix = "NVTX-2942f167cc30c5e3a44a2aecd5b0d9c07ff61a07/c/include",
        urls = tf_mirror_urls("https://github.com/NVIDIA/NVTX/archive/2942f167cc30c5e3a44a2aecd5b0d9c07ff61a07.tar.gz"),
    )

    tf_http_archive(
        name = "boringssl",
        sha256 = "9dc53f851107eaf87b391136d13b815df97ec8f76dadb487b58b2fc45e624d2c",
        strip_prefix = "boringssl-c00d7ca810e93780bd0c8ee4eea28f4f2ea4bcdc",
        urls = tf_mirror_urls("https://github.com/google/boringssl/archive/c00d7ca810e93780bd0c8ee4eea28f4f2ea4bcdc.tar.gz"),
    )

    tf_http_archive(
        name = "com_google_ortools",
        sha256 = "f6a0bd5b9f3058aa1a814b798db5d393c31ec9cbb6103486728997b49ab127bc",
        strip_prefix = "or-tools-9.11",
        patch_file = ["//third_party/ortools:ortools.patch"],
        urls = tf_mirror_urls("https://github.com/google/or-tools/archive/v9.11.tar.gz"),
        repo_mapping = {
            "@com_google_protobuf_cc": "@com_google_protobuf",
            "@eigen": "@eigen_archive",
        },
    )

    tf_http_archive(
        name = "glpk",
        sha256 = "9a5dab356268b4f177c33e00ddf8164496dc2434e83bd1114147024df983a3bb",
        build_file = "//third_party/ortools:glpk.BUILD",
        urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/ftp.gnu.org/gnu/glpk/glpk-4.52.tar.gz",
            "http://ftp.gnu.org/gnu/glpk/glpk-4.52.tar.gz",
        ],
    )

    tf_http_archive(
        name = "scip",
        sha256 = "ee221bd13a6b24738f2e74321e2efdebd6d7c603574ca6f6cb9d4472ead2c22f",
        strip_prefix = "scip-900",
        build_file = "@com_google_ortools//bazel:scip.BUILD.bazel",
        patch_file = ["@com_google_ortools//bazel:scip-v900.patch"],
        urls = tf_mirror_urls("https://github.com/scipopt/scip/archive/refs/tags/v900.tar.gz"),
    )

    tf_http_archive(
        name = "bliss",
        build_file = "//third_party/ortools:bliss.BUILD",
        sha256 = "f57bf32804140cad58b1240b804e0dbd68f7e6bf67eba8e0c0fa3a62fd7f0f84",
        urls = tf_mirror_urls("https://github.com/google/or-tools/releases/download/v9.0/bliss-0.73.zip"),
        #url = "http://www.tcs.hut.fi/Software/bliss/bliss-0.73.zip",
    )

    tf_http_archive(
        name = "pybind11",
        urls = tf_mirror_urls("https://github.com/pybind/pybind11/archive/v2.13.6.tar.gz"),
        sha256 = "e08cb87f4773da97fa7b5f035de8763abc656d87d5773e62f6da0587d1f0ec20",
        strip_prefix = "pybind11-2.13.6",
        build_file = "//third_party:pybind11.BUILD",
    )

    tf_http_archive(
        name = "pybind11_protobuf",
        urls = tf_mirror_urls("https://github.com/pybind/pybind11_protobuf/archive/f02a2b7653bc50eb5119d125842a3870db95d251.zip"),
        sha256 = "3cf7bf0f23954c5ce6c37f0a215f506efa3035ca06e3b390d67f4cbe684dce23",
        strip_prefix = "pybind11_protobuf-f02a2b7653bc50eb5119d125842a3870db95d251",
    )

    java_import_external(
        name = "junit",
        jar_sha256 = "59721f0805e223d84b90677887d9ff567dc534d7c502ca903c0c2b17f05c116a",
        jar_urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/repo1.maven.org/maven2/junit/junit/4.12/junit-4.12.jar",
            "https://repo1.maven.org/maven2/junit/junit/4.12/junit-4.12.jar",
            "https://maven.ibiblio.org/maven2/junit/junit/4.12/junit-4.12.jar",
        ],
        licenses = ["reciprocal"],  # Common Public License Version 1.0
        testonly_ = True,
        deps = ["@org_hamcrest_core"],
    )

    java_import_external(
        name = "org_hamcrest_core",
        jar_sha256 = "66fdef91e9739348df7a096aa384a5685f4e875584cce89386a7a47251c4d8e9",
        jar_urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/repo1.maven.org/maven2/org/hamcrest/hamcrest-core/1.3/hamcrest-core-1.3.jar",
            "https://repo1.maven.org/maven2/org/hamcrest/hamcrest-core/1.3/hamcrest-core-1.3.jar",
            "https://maven.ibiblio.org/maven2/org/hamcrest/hamcrest-core/1.3/hamcrest-core-1.3.jar",
        ],
        licenses = ["notice"],  # New BSD License
        testonly_ = True,
    )

    java_import_external(
        name = "com_google_testing_compile",
        jar_sha256 = "edc180fdcd9f740240da1a7a45673f46f59c5578d8cd3fbc912161f74b5aebb8",
        jar_urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/repo1.maven.org/maven2/com/google/testing/compile/compile-testing/0.11/compile-testing-0.11.jar",
            "https://repo1.maven.org/maven2/com/google/testing/compile/compile-testing/0.11/compile-testing-0.11.jar",
        ],
        licenses = ["notice"],  # New BSD License
        testonly_ = True,
        deps = ["@com_google_guava", "@com_google_truth"],
    )

    java_import_external(
        name = "com_google_truth",
        jar_sha256 = "032eddc69652b0a1f8d458f999b4a9534965c646b8b5de0eba48ee69407051df",
        jar_urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/repo1.maven.org/maven2/com/google/truth/truth/0.32/truth-0.32.jar",
            "https://repo1.maven.org/maven2/com/google/truth/truth/0.32/truth-0.32.jar",
        ],
        licenses = ["notice"],  # Apache 2.0
        testonly_ = True,
        deps = ["@com_google_guava"],
    )

    java_import_external(
        name = "org_checkerframework_qual",
        jar_sha256 = "d261fde25d590f6b69db7721d469ac1b0a19a17ccaaaa751c31f0d8b8260b894",
        jar_urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/repo1.maven.org/maven2/org/checkerframework/checker-qual/2.10.0/checker-qual-2.10.0.jar",
            "https://repo1.maven.org/maven2/org/checkerframework/checker-qual/2.10.0/checker-qual-2.10.0.jar",
        ],
        licenses = ["notice"],  # Apache 2.0
    )

    java_import_external(
        name = "com_squareup_javapoet",
        jar_sha256 = "5bb5abdfe4366c15c0da3332c57d484e238bd48260d6f9d6acf2b08fdde1efea",
        jar_urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/repo1.maven.org/maven2/com/squareup/javapoet/1.9.0/javapoet-1.9.0.jar",
            "https://repo1.maven.org/maven2/com/squareup/javapoet/1.9.0/javapoet-1.9.0.jar",
        ],
        licenses = ["notice"],  # Apache 2.0
    )

    tf_http_archive(
        name = "cython",
        build_file = "//third_party:cython.BUILD",
        sha256 = "da72f94262c8948e04784c3e6b2d14417643703af6b7bd27d6c96ae7f02835f1",
        strip_prefix = "cython-3.1.2",
        urls = tf_mirror_urls("https://github.com/cython/cython/archive/3.1.2.tar.gz"),
    )

    tf_http_archive(
        name = "build_bazel_rules_android",
        sha256 = "cd06d15dd8bb59926e4d65f9003bfc20f9da4b2519985c27e190cddc8b7a7806",
        strip_prefix = "rules_android-0.1.1",
        urls = tf_mirror_urls("https://github.com/bazelbuild/rules_android/archive/v0.1.1.zip"),
    )

    # Apple and Swift rules.
    # https://github.com/bazelbuild/rules_apple/releases
    tf_http_archive(
        name = "build_bazel_rules_apple",
        sha256 = "b4df908ec14868369021182ab191dbd1f40830c9b300650d5dc389e0b9266c8d",
        urls = tf_mirror_urls("https://github.com/bazelbuild/rules_apple/releases/download/3.5.1/rules_apple.3.5.1.tar.gz"),
    )

    # https://github.com/bazelbuild/rules_swift/releases
    tf_http_archive(
        name = "build_bazel_rules_swift",
        sha256 = "bb01097c7c7a1407f8ad49a1a0b1960655cf823c26ad2782d0b7d15b323838e2",
        urls = tf_mirror_urls("https://github.com/bazelbuild/rules_swift/releases/download/1.18.0/rules_swift.1.18.0.tar.gz"),
    )

    # https://github.com/bazelbuild/apple_support/releases
    tf_http_archive(
        name = "build_bazel_apple_support",
        sha256 = "d71b02d6df0500f43279e22400db6680024c1c439115c57a9a82e9effe199d7b",
        urls = tf_mirror_urls("https://github.com/bazelbuild/apple_support/releases/download/1.18.1/apple_support.1.18.1.tar.gz"),
    )

    # https://github.com/apple/swift-protobuf/releases
    tf_http_archive(
        name = "com_github_apple_swift_swift_protobuf",
        strip_prefix = "swift-protobuf-1.19.0/",
        sha256 = "f057930b9dbd17abeaaceaa45e9f8b3e87188c05211710563d2311b9edf490aa",
        urls = tf_mirror_urls("https://github.com/apple/swift-protobuf/archive/1.19.0.tar.gz"),
    )

    # https://github.com/google/xctestrunner/releases
    tf_http_archive(
        name = "xctestrunner",
        strip_prefix = "xctestrunner-0.2.15",
        sha256 = "b789cf18037c8c28d17365f14925f83b93b1f7dabcabb80333ae4331cf0bcb2f",
        urls = tf_mirror_urls("https://github.com/google/xctestrunner/archive/refs/tags/0.2.15.tar.gz"),
    )

    tf_http_archive(
        name = "nlohmann_json_lib",
        build_file = "//third_party:nlohmann_json.BUILD",
        sha256 = "5daca6ca216495edf89d167f808d1d03c4a4d929cef7da5e10f135ae1540c7e4",
        strip_prefix = "json-3.10.5",
        urls = tf_mirror_urls("https://github.com/nlohmann/json/archive/v3.10.5.tar.gz"),
    )

    # Dependencies required by grpc
    #   - pin rules_go to a newer version so it's compatible with Bazel 6.0
    #   - patch upb so that it's compatible with Bazel 6.0, the latest version of upb doesn't work with the old grpc version.
    tf_http_archive(
        name = "io_bazel_rules_go",
        sha256 = "16e9fca53ed6bd4ff4ad76facc9b7b651a89db1689a2877d6fd7b82aa824e366",
        urls = tf_mirror_urls("https://github.com/bazelbuild/rules_go/releases/download/v0.34.0/rules_go-v0.34.0.zip"),
    )

    tf_http_archive(
        name = "upb",
        sha256 = "61d0417abd60e65ed589c9deee7c124fe76a4106831f6ad39464e1525cef1454",
        strip_prefix = "upb-9effcbcb27f0a665f9f345030188c0b291e32482",
        patch_file = [
            "//third_party/grpc:upb_platform_fix.patch",
            # Disables warning-as-error when building upb, as it generates
            # warnings when compiled with clang.
            "//third_party/grpc:upb_build.patch",
        ],
        urls = tf_mirror_urls("https://github.com/protocolbuffers/upb/archive/9effcbcb27f0a665f9f345030188c0b291e32482.tar.gz"),
    )

    tf_http_archive(
        name = "com_github_glog_glog",
        sha256 = "f28359aeba12f30d73d9e4711ef356dc842886968112162bc73002645139c39c",
        strip_prefix = "glog-0.4.0",
        urls = tf_mirror_urls("https://github.com/google/glog/archive/refs/tags/v0.4.0.tar.gz"),
    )

    tf_http_archive(
        name = "spirv_headers",
        sha256 = "11d835c60297b26532c05c3f3b581ba7a2787b5ae7399e94f72c392169216f11",
        strip_prefix = "SPIRV-Headers-b73e168ca5e123dcf3dea8a34b19a5130f421ae1",
        urls = tf_mirror_urls("https://github.com/KhronosGroup/SPIRV-Headers/archive/b73e168ca5e123dcf3dea8a34b19a5130f421ae1.tar.gz"),
    )

    tf_http_archive(
        name = "spirv_llvm_translator",
        sha256 = "d499769f4fd1e0ce9d4dbd3622ee7e3e641b5623dcdf811521e3e7c0bdb1e6c2",
        strip_prefix = "SPIRV-LLVM-Translator-dad1f0eaab8047a4f73c50ed5f3d1694b78aae97",
        build_file = "//third_party/spirv_llvm_translator:spirv_llvm_translator.BUILD",
        patch_file = ["//third_party/spirv_llvm_translator:spirv_llvm_translator.patch"],
        urls = tf_mirror_urls("https://github.com/KhronosGroup/SPIRV-LLVM-Translator/archive/dad1f0eaab8047a4f73c50ed5f3d1694b78aae97.tar.gz"),
    )

# buildifier: disable=function-docstring
# buildifier: disable=unnamed-macro
def workspace():
    # Check the bazel version before executing any repository rules, in case
    # those rules rely on the version we require here.
    versions.check("1.0.0")

    # Initialize toolchains and platforms.
    _tf_toolchains()

    # Import third party repositories according to go/tfbr-thirdparty.
    _initialize_third_party()

    # Import all other repositories. This should happen before initializing
    # any external repositories, because those come with their own
    # dependencies. Those recursive dependencies will only be imported if they
    # don't already exist (at least if the external repository macros were
    # written according to common practice to query native.existing_rule()).
    _tf_repositories()

    bazel_features_deps()

# Alias so it can be loaded without assigning to a different symbol to prevent
# shadowing previous loads and trigger a buildifier warning.
xla_workspace2 = workspace
