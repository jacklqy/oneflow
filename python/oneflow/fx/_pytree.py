"""
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""
from typing import Callable, Any, Tuple, List, Dict, Type
from oneflow.utils._pytree import PyTree, TreeSpec, LeafSpec

FlattenFuncSpec = Callable[[PyTree, TreeSpec], List]

SUPPORTED_NODES: Dict[Type[Any], Any] = {}


def register_pytree_flatten_spec(typ: Any, flatten_fn_spec: FlattenFuncSpec) -> None:
    SUPPORTED_NODES[typ] = flatten_fn_spec


def tree_flatten_spec(pytree: PyTree, spec: TreeSpec) -> List[Any]:
    if isinstance(spec, LeafSpec):
        return [pytree]
    if spec.type not in SUPPORTED_NODES:
        raise RuntimeError(
            f"{type(pytree)} does not have a flatten_fn_spec associated with it. Please register one with"
            "oneflow.fx._pytree.register_pytree_flatten_spec.  If you have serialized your model, make"
            "sure that any custom pytrees have been registered before loading it."
        )
    flatten_fn_spec = SUPPORTED_NODES[spec.type]
    child_pytrees = flatten_fn_spec(pytree, spec)
    result = []
    for child, child_spec in zip(child_pytrees, spec.children_specs):
        flat = tree_flatten_spec(child, child_spec)
        result += flat
    return result


def _dict_flatten_spec(d: Dict[Any, Any], spec: TreeSpec) -> List[Any]:
    return list([d[k] for k in spec.context])


def _list_flatten_spec(d: List[Any], spec: TreeSpec) -> List[Any]:
    return [d[i] for i in range(len(spec.children_specs))]


def _tuple_flatten_spec(d: Tuple[Any], spec: TreeSpec) -> List[Any]:
    return [d[i] for i in range(len(spec.children_specs))]


register_pytree_flatten_spec(dict, _dict_flatten_spec)
register_pytree_flatten_spec(list, _list_flatten_spec)
register_pytree_flatten_spec(tuple, _tuple_flatten_spec)