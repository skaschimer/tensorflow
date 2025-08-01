// RUN: emitters_opt %s -split-input-file -xla-lower-tensors -xla-merge-pointers | FileCheck %s

module {
  func.func private @tensorargs(%arg0: tensor<43xf32> {xla.slice_index = 0},
                        %arg1: tensor<43xf32> {xla.slice_index = 1, xla.invariant},
                        %arg2: tensor<43xf32> {xla.slice_index = 0},
                        %arg3: index) -> f32 {
    %v0 = tensor.extract %arg0[%arg3] : tensor<43xf32>
    %v1 = tensor.extract %arg1[%arg3] : tensor<43xf32>
    %v2 = tensor.extract %arg2[%arg3] : tensor<43xf32>
    %sum = arith.addf %v0, %v1 : f32
    %sum2 = arith.addf %sum, %v2 : f32
    func.return %sum2 : f32
  }

  func.func @tensorcall(%arg0: tensor<43xf32> {xla.slice_index = 0},
                        %arg1: tensor<43xf32> {xla.slice_index = 1, xla.invariant},
                        %arg2: tensor<43xf32> {xla.slice_index = 0},
                        %arg3: index) -> f32 attributes {xla.entry} {
    %call = func.call @tensorargs(%arg0, %arg1, %arg2, %arg3) :
      (tensor<43xf32>, tensor<43xf32>, tensor<43xf32>, index) -> f32
    func.return %call : f32
  }
}

// CHECK:      func.func private @tensorargs(
// CHECK-SAME:   %[[ARG0:.*]]: !llvm.ptr {llvm.noalias},
// CHECK-SAME:   %[[ARG1:.*]]: !llvm.ptr {llvm.noalias, xla.invariant},
// CHECK-SAME:   %[[ARG2:.*]]: index) -> f32 {
// CHECK:        %[[GEP0:.*]] = llvm.getelementptr inbounds %[[ARG0]]
// CHECK:        llvm.load %[[GEP0]] : !llvm.ptr
// CHECK:        %[[GEP1:.*]] = llvm.getelementptr inbounds %[[ARG1]]
// CHECK:        llvm.load %[[GEP1]] invariant : !llvm.ptr
// CHECK:        %[[GEP2:.*]] = llvm.getelementptr inbounds %[[ARG0]]

// CHECK:      func.func @tensorcall
// CHECK-SAME:   %[[ARG0:[a-z0-9]+]]: !llvm.ptr {llvm.noalias},
// CHECK-SAME:   %[[ARG1:[a-z0-9]+]]: !llvm.ptr {llvm.noalias, xla.invariant},
// CHECK-SAME:   %[[ARG2:[a-z0-9]+]]: !llvm.ptr {llvm.noalias},
// CHECK-SAME:   %[[ARG3:[a-z0-9]+]]: index) -> f32 attributes {xla.entry} {
// CHECK:        call @tensorargs(%[[ARG0]], %[[ARG1]], %[[ARG3]])

// -----

func.func @wrapping_func(%arg0: tensor<43xf32>,
                         %arg1: tensor<43xf32>,
                         %idx: index) -> f32 {
  %result = func.call @private_entry_func(%arg0, %arg1, %idx) :
      (tensor<43xf32>, tensor<43xf32>, index) -> f32
  func.return %result : f32
}

func.func private @private_entry_func(
  %arg0: tensor<43xf32> {xla.slice_index = 0},
  %arg1: tensor<43xf32> {xla.slice_index = 0},
  %idx: index) -> f32 attributes {xla.entry} {
  %v0 = tensor.extract %arg0[%idx] : tensor<43xf32>
  %v1 = tensor.extract %arg1[%idx] : tensor<43xf32>
  %sum = arith.addf %v0, %v1 : f32
  func.return %sum : f32
}

// CHECK-LABEL: func.func @wrapping_func(
// CHECK-SAME:    %[[ARG0:[a-z0-9]+]]: !llvm.ptr,
// CHECK-SAME:    %[[ARG1:[a-z0-9]+]]: !llvm.ptr,
// CHECK-SAME:    %[[IDX:[a-z0-9]+]]: index) -> f32 {
// CHECK:         %[[RESULT:.*]] = call @private_entry_func(
// CHECK-SAME:      %[[ARG0]], %[[ARG1]], %[[IDX]])
// CHECK:         return %[[RESULT]] : f32
// CHECK:       }

// CHECK-LABEL: func.func private @private_entry_func(
// CHECK-SAME:    %[[ARG0:[a-z0-9]+]]: !llvm.ptr {llvm.noalias},
// CHECK-SAME:    %[[ARG1:[a-z0-9]+]]: !llvm.ptr {llvm.noalias},
// CHECK-SAME:    %[[IDX:[a-z0-9]+]]: index) -> f32 attributes {xla.entry} {
// CHECK:         %[[GEP0:.*]] = llvm.getelementptr inbounds %[[ARG0]]
// CHECK:         %[[V0:.*]] = llvm.load %[[GEP0]]
// CHECK:         %[[GEP1:.*]] = llvm.getelementptr inbounds %[[ARG0]]
// CHECK:         %[[V1:.*]] = llvm.load %[[GEP1]]
// CHECK:         %[[SUM:.*]] = arith.addf %[[V0]], %[[V1]]
// CHECK:         return %[[SUM]]
// CHECK:        }
