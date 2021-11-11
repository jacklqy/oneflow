# JIT in OneFlow

## Summary

There will be two modes of JIT in OneFlow:

- `oneflow.jit.trace`: `torch.jit.trace` behavior, create a JIT module from a vanilla `nn.module`
- `oneflow.jit.exec`: "Lazy Tensor" behavior

## Minimum Examples

- ### `oneflow.jit.trace`

  ```python
  class MyModule(oneflow.nn.Module):
      def __init__(self, N, M):
          super(MyModule, self).__init__()
          self.weight = oneflow.nn.Parameter(oneflow.rand(N, M))
          self.linear = oneflow.nn.Linear(N, M)

      def forward(self, input):  # python forward will not be run after the tracing
          output = self.linear(input)
          print(output) # will print Lazy tensor with no data, in Pytorch it prints tensor with data
          return output


  linear = oneflow.jit.trace(MyModule(2, 3))
  print(linear(oneflow.randn(2, 2))) # will print eager tensor with actual data
  ```

- ### `oneflow.jit.exec` decorator

  ```python
  @oneflow.jit.exec
  def any_function(a, b):
      return a + b

  class MyModule(oneflow.nn.Module):
      def __init__(self, N, M):
          super(MyModule, self).__init__()
          self.weight = oneflow.nn.Parameter(oneflow.rand(N, M))
          self.linear = oneflow.nn.Linear(N, M)

      @oneflow.jit.exec
      def forward(self, input): # python forward will be run every time the module it is called
          output = self.linear(input)
          print(output)
          return output


  linear = MyModule(2, 3)
  print(linear(oneflow.randn(2, 2))) # will print eager tensor with actual data
  ```

- ### `oneflow.jit.exec` global mode

  ```python
  oneflow.jit.exec()
  a = oneflow.randn(2, 2)
  b = oneflow.randn(2, 2)
  c = a + b # no evaluation
  d = c + a + b # no evaluation
  print(c) # evaluation starts here
  ```

## Internal Details

There are mainly three components in the JIT system:

- JIT interpreter: a special interpreter works on eager inputs and lazy intermediate tensors. It might convert
- Importer: convert OneFlow's representation to MLIR and and vice versa.
- Executor: three types of executor under development or consideration
  - Re-dispatch: convert every MLIR op to one User Op and have eager interpreter dispatch them. 10% performance boost over pure eager mode is expected. This will be used for `oneflow.jit.trace` mainly.
  - Batched-Op-Kernel: convert all MLIR ops to one UserOp and Kernel. This will be used to support CUDA graph.
  - Direct-Kernel-launch: generate and launch kernel directly.

### Sequence of operations

```cpp
struct Op {
op_expr_seq: OpExp
tensor_types: List<DataType>
}
using SeqOfOp = List<Op>
using PyUses = List<Bool> // size of py_uses is the sum of tensor_types' sizes
```

For a typical "lazy tensor" implementation, there is usually a "sequence of operations" representation. At this point, we use Op Expr to form the sequence. Every time a new Op Expr is applied in JIT interpreter, we insert new element into the sequence and combine its hash with the existing sequence's. When an evaluation is triggered, we combine the hash of the collected sequence and `PyUses` to loop up the cached MLIR.

### Tracking Python reference of an Tensor

In exec mode, it is necessary to track the if an Lazy Tensor is being reference by Python and later it will be replace with Eager Tensor.

- Use weak ptr to check if a lazy tensor is being referenced in Python.

### Multi level cache

There are four level of cache

- Python forward function -> MLIR (trace mode only)
- Op Expr -> MLIR (trace mode and exec mode)
- Vanilla MLIR -> Optimized MLIR (trace mode and exec mode)
- MLIR Op -> Op Expression (only for re-dispatch execution)