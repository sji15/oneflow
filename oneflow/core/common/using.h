/*
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
*/
#ifndef ONEFLOW_CORE_COMMON_USING_
#define ONEFLOW_CORE_COMMON_USING_

#include <unordered_set>
#include <unordered_map>
namespace oneflow {

template<typename Key, typename T, typename Hash = std::hash<Key>>
using HashMap = std::unordered_map<Key, T, Hash>;

template<typename Key, typename Hash = std::hash<Key>>
using HashSet = std::unordered_set<Key, Hash>;
#define USING_HASH_CONTAINER

}  // namespace oneflow

#endif  // ONEFLOW_CORE_COMMON_USING_
