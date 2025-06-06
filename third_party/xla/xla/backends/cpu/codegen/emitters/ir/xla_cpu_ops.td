/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_BACKENDS_CPU_CODEGEN_EMITTERS_IR_XLA_CPU_OPS
#define XLA_BACKENDS_CPU_CODEGEN_EMITTERS_IR_XLA_CPU_OPS

include "mlir/IR/OpBase.td"
include "xla/backends/cpu/codegen/emitters/ir/xla_cpu_dialect.td"
include "xla/backends/cpu/codegen/emitters/ir/xla_cpu_types.td"

class XLACPU_Op<string mnemonic, list<Trait> traits = []> :
      Op<XlaCpuDialect, mnemonic, traits> {
}

//===----------------------------------------------------------------------===//
// !xla_cpu.load
//===----------------------------------------------------------------------===//

def XLACPU_LoadOp : XLACPU_Op<"load"> {
  let summary = "Loads a tensor from an XLA:CPU call frame";

  let description = [{
    Loads a tensor from an XLA:CPU call frame at the given index.

    ```mlir
    %0 = xla_cpu.load %call_frame, 0 : tensor<32x32xf32>
    ```
  }];

  let arguments = (ins XLACPU_CallFrame:$call_frame,
                       I32Attr:$index);

  let results = (outs AnyStaticShapeTensor:$result);

  let assemblyFormat = [{
    $call_frame `,` $index attr-dict `:` type($result)
  }];
}

//===----------------------------------------------------------------------===//
// !xla_cpu.success
//===----------------------------------------------------------------------===//

def XLACPU_SuccessOp : XLACPU_Op<"success"> {
  let summary = "Creates an !xla_cpu.error in the 'success' state.";
  let arguments = (ins);
  let results = (outs XLACPU_Error:$result);
  let assemblyFormat = " attr-dict `:` type($result) ";
}

#endif // XLA_BACKENDS_CPU_CODEGEN_EMITTERS_IR_XLA_CPU_OPS
