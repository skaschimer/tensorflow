# DTensor Python API and libraries.

load("//tensorflow:pytype.default.bzl", "pytype_strict_library")
load("//tensorflow:strict.default.bzl", "py_strict_library")
load("//tensorflow:tensorflow.bzl", "tf_gen_op_wrapper_py")

default_visibility = [
    "//tensorflow/dtensor:dtensor-internal",
    "//third_party/py/jax_tpu_embedding:__subpackages__",
]

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = default_visibility,
    licenses = ["notice"],
)

# -----------------------------------------------------------------------------
# A list of all modules linked into TensorFlow.

py_strict_library(
    name = "dtensor",
    srcs = ["__init__.py"],
    visibility = [
        "//tensorflow/python:__pkg__",
        "//tensorflow/python/tools/api/generator:__pkg__",
    ],
    deps = [
        ":accelerator_util",
        ":api",
        ":config",
        ":d_checkpoint",
        ":d_variable",
        ":input_util",
        ":layout",
        ":mesh_util",
        ":save_restore",
        ":tpu_util",
    ],
)

# -----------------------------------------------------------------------------
# Implementations of the public API.

pytype_strict_library(
    name = "api",
    srcs = ["api.py"],
    deps = [
        ":dtensor_device",
        ":gen_dtensor_ops",
        ":layout",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:tensor",
        "//tensorflow/python/util:deprecation",
        "//tensorflow/python/util:tf_export",
    ],
)

pytype_strict_library(
    name = "config",
    srcs = ["config.py"],
    deps = [
        "//tensorflow/python/eager:context",
        "//tensorflow/python/framework:config",
        "//tensorflow/python/framework:device",
        "//tensorflow/python/util:tf_export",
    ],
)

tf_gen_op_wrapper_py(
    name = "gen_dtensor_ops",
    out = "gen_dtensor_ops.py",
    extra_py_deps = [
        "//tensorflow/python:pywrap_tfe",
        "//tensorflow/python/util:dispatch",
        "//tensorflow/python/util:deprecation",
        "//tensorflow/python/util:tf_export",
    ],
    py_lib_rule = py_strict_library,
    deps = [
        "//tensorflow/dtensor/cc:dtensor_ops",
        "//tensorflow/dtensor/cc:dtensor_tpu_ops",
    ],
)

pytype_strict_library(
    name = "layout",
    srcs = ["layout.py"],
    deps = [
        "//tensorflow/dtensor/proto:layout_proto_py_pb2",
        "//tensorflow/python:_pywrap_dtensor_device",
        "//tensorflow/python/framework:device",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:tensor",
        "//tensorflow/python/util:tf_export",
        "//third_party/py/numpy",
    ],
)

pytype_strict_library(
    name = "d_random",
    srcs = ["d_random.py"],
    deps = [
        ":api",
        ":layout",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/framework:dtypes",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:tensor_shape",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/ops:math_ops",
        "//tensorflow/python/ops:math_ops_gen",
        "//tensorflow/python/ops:shape_util",
        "//tensorflow/python/ops:stateless_random_ops_gen",
    ],
)

pytype_strict_library(
    name = "d_variable",
    srcs = ["d_variable.py"],
    deps = [
        ":api",
        ":layout",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/eager:def_function",
        "//tensorflow/python/framework:dtypes",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/ops:math_ops",
        "//tensorflow/python/ops:resource_variable_ops",
        "//tensorflow/python/trackable:base",
        "//tensorflow/python/training/saving:saveable_object",
        "//tensorflow/python/util:tf_export",
    ],
)

pytype_strict_library(
    name = "d_checkpoint",
    srcs = ["d_checkpoint.py"],
    deps = [
        ":api",
        ":d_variable",
        ":gen_dtensor_ops",
        ":layout",
        ":save_restore",
        "//tensorflow/core:protos_all_py",
        "//tensorflow/dtensor/proto:layout_proto_py_pb2",
        "//tensorflow/python/checkpoint",
        "//tensorflow/python/checkpoint:checkpoint_options",
        "//tensorflow/python/checkpoint:graph_view",
        "//tensorflow/python/checkpoint:restore",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/framework:constant_op",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/trackable:base",
        "//tensorflow/python/trackable:data_structures",
        "//tensorflow/python/training:py_checkpoint_reader",
        "//tensorflow/python/training/saving:saveable_object",
        "//tensorflow/python/training/saving:saveable_object_util",
        "//tensorflow/python/util:deprecation",
        "//tensorflow/python/util:nest",
        "//tensorflow/python/util:tf_export",
    ],
)

pytype_strict_library(
    name = "numpy_util",
    srcs = ["numpy_util.py"],
    deps = [
        ":api",
        ":layout",
        "//tensorflow/python/eager/polymorphic_function",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/ops:array_ops_stack",
        "//tensorflow/python/ops:sparse_ops",
        "//tensorflow/python/ops:stateless_random_ops",
        "//tensorflow/python/types:core",
        "//third_party/py/numpy",
    ],
)

pytype_strict_library(
    name = "save_restore",
    srcs = ["save_restore.py"],
    deps = [
        ":api",
        ":d_variable",
        ":gen_dtensor_ops",
        ":layout",
        ":mesh_util",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:tensor",
        "//tensorflow/python/ops:io_ops",
        "//tensorflow/python/ops:variables",
        "//tensorflow/python/util:tf_export",
    ],
)

# -----------------------------------------------------------------------------
# The DTensor runtime.

pytype_strict_library(
    name = "dtensor_device",
    srcs = ["dtensor_device.py"],
    deps = [
        ":config",
        ":gen_dtensor_ops",
        ":layout",
        "//tensorflow/core:protos_all_py",
        "//tensorflow/python:_pywrap_dtensor_device",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/eager:core",
        "//tensorflow/python/framework:device",
        "//tensorflow/python/framework:dtypes",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:sparse_tensor",
        "//tensorflow/python/framework:tensor",
        "//tensorflow/python/framework:tensor_util",
        "//tensorflow/python/util:_pywrap_utils",
        "//third_party/py/numpy",
    ],
)

# -----------------------------------------------------------------------------
# Utilities.

pytype_strict_library(
    name = "mesh_util",
    srcs = ["mesh_util.py"],
    visibility = default_visibility + [
        "//tensorflow/dtensor:dtensor-users",
    ],
    deps = [
        ":accelerator_util",
        ":api",
        ":config",
        ":layout",
        ":tpu_util",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/framework:config",
        "//tensorflow/python/framework:device",
        "//tensorflow/python/framework:tfrt_utils",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/ops:math_ops",
        "//tensorflow/python/util:tf_export",
        "//third_party/py/numpy",
        "@absl_py//absl/flags",
        "@absl_py//absl/logging",
    ],
)

# TODO(b/245589661): Split accelerator_util to its module after
# The circular dependence is removed with dtensor_initialize_tpu_system.
pytype_strict_library(
    name = "tpu_util",
    srcs = [
        "accelerator_util.py",
        "tpu_util.py",
    ],
    visibility = default_visibility + [
        "//tensorflow/dtensor:dtensor-users",
    ],
    deps = [
        ":config",
        ":dtensor_device",
        ":gen_dtensor_ops",
        ":layout",
        "//tensorflow/core:protos_all_py",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/eager:def_function",
        "//tensorflow/python/framework:config",
        "//tensorflow/python/framework:constant_op",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/ops:math_ops",
        "//tensorflow/python/platform:remote_utils",
        "//tensorflow/python/platform:tf_logging",
        "//tensorflow/python/tpu:topology",
        "//tensorflow/python/util:numpy_compat",
        "//tensorflow/python/util:tf_export",
        "//third_party/py/numpy",
        "@absl_py//absl/logging",
    ],
)

pytype_strict_library(
    name = "heartbeat",
    srcs = ["heartbeat.py"],
    deps = [
        ":api",
        ":config",
        "//tensorflow/python/framework:constant_op",
        "//tensorflow/python/framework:device",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/ops:collective_ops",
        "//tensorflow/python/platform:tf_logging",
        "//third_party/py/numpy",
    ],
)

pytype_strict_library(
    name = "accelerator_util",
    srcs = [],
    deps = [
        ":tpu_util",
    ],
)

pytype_strict_library(
    name = "input_util",
    srcs = ["input_util.py"],
    deps = [
        ":api",
        ":config",
        ":layout",
        "//tensorflow/python/data/experimental/ops:data_service_ops",
        "//tensorflow/python/data/experimental/ops:distribute",
        "//tensorflow/python/data/ops:dataset_ops",
        "//tensorflow/python/data/ops:iterator_ops",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/framework:constant_op",
        "//tensorflow/python/framework:dtypes",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:tensor",
        "//tensorflow/python/framework:tensor_shape",
        "//tensorflow/python/framework:tensor_spec",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/ops:math_ops",
        "//tensorflow/python/types:data",
        "//tensorflow/python/util:nest",
        "//tensorflow/python/util:tf_export",
    ],
)
