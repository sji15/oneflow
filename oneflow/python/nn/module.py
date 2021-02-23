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

from __future__ import absolute_import
from collections import OrderedDict, namedtuple
from typing import Union, TypeVar, Iterator, Optional, Set, Tuple, Dict, List
from inspect import signature
import itertools

from oneflow.python.oneflow_export import oneflow_export
from oneflow.python.framework.ops import parallel_cast
from oneflow.python.ops.get_variable import api_get_variable as get_variable
from oneflow.python.ops import initializer_util


# See https://mypy.readthedocs.io/en/latest/generics.html#generic-methods-and-generic-self for the use
# of `T` to annotate `self`. Many methods of `Module` return `self` and we want those return values to be
# the type of the subclass, not the looser type of `Module`.
T = TypeVar("T", bound="Module")


def new_counter():
    _counter = 0

    def counter():
        nonlocal _counter
        while True:
            yield _counter
            _counter += 1

    return counter()


counter = new_counter()


class Tensor:
    pass


class InputConfigs:
    def __init__(self):
        self._configs = {}

    def __delitem__(self, key):
        del self._configs[key]

    def __getitem__(self, key):
        return self._configs[key]

    def __setitem__(self, key, value):
        self._configs[key] = value

    def _to_dict(self):
        return self._configs


# TODO: rename
@oneflow_export("nn.Module_v2")
class Module(object):
    def __init__(self):
        self.training = True
        self.consistent = False
        self._parameters = OrderedDict()
        self._buffers = OrderedDict()
        self._non_persistent_buffers_set = set()
        self._backward_hooks = OrderedDict()
        self._is_full_backward_hook = None
        self._forward_hooks = OrderedDict()
        self._forward_pre_hooks = OrderedDict()
        self._state_dict_hooks = OrderedDict()
        self._load_state_dict_pre_hooks = OrderedDict()
        self._modules = OrderedDict()
        self.input_configs = InputConfigs()

    def forward(self, *args):
        raise NotImplementedError()

    def consistent_forward(self, *args):
        return self.forward(*args)

    def force_mirrored_forward(self, *args):
        raise NotImplementedError()

    def __call__(self, *args):
        for hook in itertools.chain(self._forward_pre_hooks.values()):
            result = hook(self, args)
            if result is not None:
                if not isinstance(result, tuple):
                    result = (result,)
                args = result

        if self.consistent:
            is_force_mirrored_overloaded = (
                Module.__dict__["force_mirrored_forward"]
                != self.__class__.__dict__["force_mirrored_forward"]
            )
            if is_force_mirrored_overloaded:
                return self.force_mirrored_forward(*args)
            else:
                print(self.input_configs._to_dict())
                args = list(args)
                for key, value in self.input_configs._to_dict().items():
                    args[key] = parallel_cast(args[key], distribute=value)
                return self.consisten_forward(*args)
        else:
            return self.forward(*args)

    def register_buffer(
        self, name: str, tensor: Optional[Tensor], persistent: bool = True
    ) -> None:
        if "_buffers" not in self.__dict__:
            raise AttributeError("cannot assign buffer before Module.__init__() call")
        # elif not isinstance(name, torch._six.string_classes):
        #     raise TypeError("buffer name should be a string. "
        #                     "Got {}".format(torch.typename(name)))
        elif "." in name:
            raise KeyError('buffer name can\'t contain "."')
        elif name == "":
            raise KeyError('buffer name can\'t be empty string ""')
        elif hasattr(self, name) and name not in self._buffers:
            raise KeyError("attribute '{}' already exists".format(name))
        elif tensor is not None and not isinstance(tensor, Tensor):
            raise TypeError(
                "cannot assign '{}' object to buffer '{}' "
                "(Tensor or None required)".format(type(tensor), name)
            )
        else:
            self._buffers[name] = tensor
            if persistent:
                self._non_persistent_buffers_set.discard(name)
            else:
                self._non_persistent_buffers_set.add(name)

    def register_parameter(self, name: str, param: Optional["Parameter"]) -> None:
        if "_parameters" not in self.__dict__:
            raise AttributeError(
                "cannot assign parameter before Module.__init__() call"
            )

        # elif not isinstance(name, torch._six.string_classes):
        #     raise TypeError("parameter name should be a string. "
        #                     "Got {}".format(torch.typename(name)))
        elif "." in name:
            raise KeyError('parameter name can\'t contain "."')
        elif name == "":
            raise KeyError('parameter name can\'t be empty string ""')
        elif hasattr(self, name) and name not in self._parameters:
            raise KeyError("attribute '{}' already exists".format(name))

        if param is None:
            self._parameters[name] = None
        elif not isinstance(param, Parameter):
            raise TypeError(
                "cannot assign '{}' object to parameter '{}' "
                "(nn.Parameter or None required)".format(type(param), name)
            )
        # elif param.grad_fn:
        #     raise ValueError(
        #         "Cannot assign non-leaf Tensor to parameter '{0}'. Model "
        #         "parameters must be created explicitly. To express '{0}' "
        #         "as a function of another Tensor, compute the value in "
        #         "the forward() method.".format(name))
        else:
            self._parameters[name] = param

    def __getattr__(self, name: str) -> Union[Tensor, "Module"]:
        if "_parameters" in self.__dict__:
            _parameters = self.__dict__["_parameters"]
            if name in _parameters:
                return _parameters[name]
        if "_buffers" in self.__dict__:
            _buffers = self.__dict__["_buffers"]
            if name in _buffers:
                return _buffers[name]
        if "_modules" in self.__dict__:
            modules = self.__dict__["_modules"]
            if name in modules:
                return modules[name]
        raise AttributeError(
            "'{}' object has no attribute '{}'".format(type(self).__name__, name)
        )

    def __setattr__(self, name: str, value: Union[Tensor, "Module"]) -> None:
        def remove_from(*dicts_or_sets):
            for d in dicts_or_sets:
                if name in d:
                    if isinstance(d, dict):
                        del d[name]
                    else:
                        d.discard(name)

        params = self.__dict__.get("_parameters")
        if isinstance(value, Parameter):
            if params is None:
                raise AttributeError(
                    "cannot assign parameters before Module.__init__() call"
                )
            remove_from(
                self.__dict__,
                self._buffers,
                self._modules,
                self._non_persistent_buffers_set,
            )
            self.register_parameter(name, value)
        elif params is not None and name in params:
            if value is not None:
                raise TypeError(
                    "cannot assign '{}' as parameter '{}' "
                    "(nn.Parameter or None expected)".format(type(value), name)
                )
            self.register_parameter(name, value)
        else:
            modules = self.__dict__.get("_modules")
            if isinstance(value, Module):
                if modules is None:
                    raise AttributeError(
                        "cannot assign module before Module.__init__() call"
                    )
                remove_from(
                    self.__dict__,
                    self._parameters,
                    self._buffers,
                    self._non_persistent_buffers_set,
                )
                modules[name] = value
            elif modules is not None and name in modules:
                if value is not None:
                    raise TypeError(
                        "cannot assign '{}' as child module '{}' "
                        "(nn.Module or None expected)".format(type(value), name)
                    )
                modules[name] = value
            else:
                buffers = self.__dict__.get("_buffers")
                if buffers is not None and name in buffers:
                    if value is not None and not isinstance(value, Tensor):
                        raise TypeError(
                            "cannot assign '{}' as buffer '{}' "
                            "(Tensor or None expected)".format(type(value), name)
                        )
                    buffers[name] = value
                else:
                    object.__setattr__(self, name, value)

    def _named_members(self, get_members_fn, prefix="", recurse=True):
        memo = set()
        modules = self.named_modules(prefix=prefix) if recurse else [(prefix, self)]
        for module_prefix, module in modules:
            members = get_members_fn(module)
            for k, v in members:
                if v is None or v in memo:
                    continue
                memo.add(v)
                name = module_prefix + ("." if module_prefix else "") + k
                yield name, v

    def parameters(self, recurse: bool = True) -> Iterator["Parameter"]:
        for name, param in self.named_parameters(recurse=recurse):
            yield param

    def named_parameters(
        self, prefix: str = "", recurse: bool = True
    ) -> Iterator[Tuple[str, Tensor]]:
        gen = self._named_members(
            lambda module: module._parameters.items(), prefix=prefix, recurse=recurse
        )
        for elem in gen:
            yield elem

    def buffers(self, recurse: bool = True) -> Iterator[Tensor]:
        for name, buf in self.named_buffers(recurse=recurse):
            yield buf

    def named_buffers(
        self, prefix: str = "", recurse: bool = True
    ) -> Iterator[Tuple[str, Tensor]]:
        gen = self._named_members(
            lambda module: module._buffers.items(), prefix=prefix, recurse=recurse
        )
        for elem in gen:
            yield elem

    def children(self) -> Iterator["Module"]:
        for name, module in self.named_children():
            yield module

    def named_children(self) -> Iterator[Tuple[str, "Module"]]:
        memo = set()
        for name, module in self._modules.items():
            if module is not None and module not in memo:
                memo.add(module)
                yield name, module

    def modules(self) -> Iterator["Module"]:
        for name, module in self.named_modules():
            yield module

    def named_modules(self, memo: Optional[Set["Module"]] = None, prefix: str = ""):
        if memo is None:
            memo = set()
        if self not in memo:
            memo.add(self)
            yield prefix, self
            for name, module in self._modules.items():
                if module is None:
                    continue
                submodule_prefix = prefix + ("." if prefix else "") + name
                for m in module.named_modules(memo, submodule_prefix):
                    yield m

    def train(self: T, mode: bool = True) -> T:
        self.training = mode
        for module in self.children():
            module.train(mode)
        return self

    def eval(self: T) -> T:
        return self.train(False)

    def _save_to_state_dict(self, destination, prefix, keep_vars):
        for name, param in self._parameters.items():
            if param is not None:
                # TODO: uncomment these line when tensor is ready
                # destination[prefix + name] = param if keep_vars else param.detach()
                destination[prefix + name] = param
        for name, buf in self._buffers.items():
            if buf is not None and name not in self._non_persistent_buffers_set:
                # destination[prefix + name] = buf if keep_vars else buf.detach()
                destination[prefix + name] = buf

    def state_dict(
        self, destination=None, prefix="", keep_vars=False
    ) -> Dict[str, Tensor]:
        if destination is None:
            destination = OrderedDict()
            destination._metadata = OrderedDict()
        # TODO:
        # destination._metadata[prefix[:-1]] = local_metadata = dict(version=self._version)

        self._save_to_state_dict(destination, prefix, keep_vars)
        for name, module in self._modules.items():
            if module is not None:
                module.state_dict(destination, prefix + name + ".", keep_vars=keep_vars)
        for hook in self._state_dict_hooks.values():
            # hook_result = hook(self, destination, prefix, local_metadata)
            hook_result = hook(self, destination, prefix)
            if hook_result is not None:
                destination = hook_result
        return destination

    def register_forward_pre_hook(self, hook: Callable[..., None]) -> None:
        self._forward_pre_hooks[len(self._forward_pre_hooks)] = hook


# placeholder
@oneflow_export("nn.Variable")
class Variable(Module):
    def __init__(self, shape):
        super().__init__()
        self.var_name = str(next(counter))
        self.shape = shape

    def forward(self):
        return get_variable(
            self.var_name, self.shape, initializer=initializer_util.ones_initializer()
        )


# placeholder
@oneflow_export("nn.Parameter")
class Parameter(Variable):
    def __init__(self, shape):
        super().__init__(shape)