// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <ray/api/common_types.h>
#include <ray/api/serializer.h>

#include <boost/callable_traits.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>

#include "absl/utility/utility.h"
#include "ray/core.h"

namespace ray {
namespace internal {

template <typename T>
inline static absl::enable_if_t<!std::is_pointer<T>::value, msgpack::sbuffer>
PackReturnValue(T result) {
  return ray::api::Serializer::Serialize(std::move(result));
}

template <typename T>
inline static absl::enable_if_t<std::is_pointer<T>::value, msgpack::sbuffer>
PackReturnValue(T result) {
  return ray::api::Serializer::Serialize((uint64_t)result);
}

inline static msgpack::sbuffer PackVoid() {
  return ray::api::Serializer::Serialize(msgpack::type::nil_t());
}

msgpack::sbuffer PackError(std::string error_msg);

template <typename>
struct RemoveFirst;

template <class First, class... Second>
struct RemoveFirst<std::tuple<First, Second...>> {
  using type = std::tuple<Second...>;
};

template <class Tuple>
using RemoveFirst_t = typename RemoveFirst<Tuple>::type;

template <typename>
struct RemoveReference;

template <class... T>
struct RemoveReference<std::tuple<T...>> {
  using type = std::tuple<absl::remove_const_t<absl::remove_reference_t<T>>...>;
};

template <class Tuple>
using RemoveReference_t = typename RemoveReference<Tuple>::type;

/// It's help to invoke functions and member functions, the class Invoker<Function> help
/// do type erase.
template <typename Function>
struct Invoker {
  /// Invoke functions by networking stream, at first deserialize the binary data to a
  /// tuple, then call function with tuple.
  static inline msgpack::sbuffer Apply(const Function &func,
                                       const std::vector<msgpack::sbuffer> &args_buffer) {
    using RetrunType = boost::callable_traits::return_type_t<Function>;
    using ArgsTuple = RemoveReference_t<boost::callable_traits::args_t<Function>>;
    if (std::tuple_size<ArgsTuple>::value != args_buffer.size()) {
      return PackError("Arguments number not match");
    }

    msgpack::sbuffer result;
    ArgsTuple tp{};
    try {
      bool is_ok =
          GetArgsTuple(tp, args_buffer,
                       absl::make_index_sequence<std::tuple_size<ArgsTuple>::value>{});
      if (!is_ok) {
        return PackError("arguments error");
      }

      result = Invoker<Function>::Call<RetrunType>(func, std::move(tp));
    } catch (msgpack::type_error &e) {
      result = PackError(std::string("invalid arguments: ") + e.what());
    } catch (const std::exception &e) {
      result = PackError(std::string("function execute exception: ") + e.what());
    } catch (...) {
      result = PackError("unknown exception");
    }

    return result;
  }

  static inline msgpack::sbuffer ApplyMember(
      const Function &func, msgpack::sbuffer *ptr,
      const std::vector<msgpack::sbuffer> &args_buffer) {
    using RetrunType = boost::callable_traits::return_type_t<Function>;
    using ArgsTuple =
        RemoveReference_t<RemoveFirst_t<boost::callable_traits::args_t<Function>>>;
    if (std::tuple_size<ArgsTuple>::value != args_buffer.size()) {
      return PackError("Arguments number not match");
    }

    msgpack::sbuffer result;
    ArgsTuple tp{};
    try {
      bool is_ok =
          GetArgsTuple(tp, args_buffer,
                       absl::make_index_sequence<std::tuple_size<ArgsTuple>::value>{});
      if (!is_ok) {
        return PackError("arguments error");
      }

      uint64_t actor_ptr =
          ray::api::Serializer::Deserialize<uint64_t>(ptr->data(), ptr->size());
      using Self = boost::callable_traits::class_of_t<Function>;
      Self *self = (Self *)actor_ptr;
      result = Invoker<Function>::CallMember<RetrunType>(func, self, std::move(tp));
    } catch (msgpack::type_error &e) {
      result = PackError(std::string("invalid arguments: ") + e.what());
    } catch (const std::exception &e) {
      result = PackError(std::string("function execute exception: ") + e.what());
    } catch (...) {
      result = PackError("unknown exception");
    }

    return result;
  }

 private:
  template <typename T>
  static inline T ParseArg(char *data, size_t size, bool &is_ok) {
    auto pair = ray::api::Serializer::DeserializeWhenNil<T>(data, size);
    is_ok = pair.first;
    return pair.second;
  }

  static inline bool GetArgsTuple(std::tuple<> &tup,
                                  const std::vector<msgpack::sbuffer> &args_buffer,
                                  absl::index_sequence<>) {
    return true;
  }

  template <size_t... I, typename... Args>
  static inline bool GetArgsTuple(std::tuple<Args...> &tp,
                                  const std::vector<msgpack::sbuffer> &args_buffer,
                                  absl::index_sequence<I...>) {
    bool is_ok = true;
    (void)std::initializer_list<int>{
        (std::get<I>(tp) = ParseArg<Args>((char *)args_buffer.at(I).data(),
                                          args_buffer.at(I).size(), is_ok),
         0)...};
    return is_ok;
  }

  template <typename R, typename F, typename... Args>
  static absl::enable_if_t<std::is_void<R>::value, msgpack::sbuffer> Call(
      const F &f, std::tuple<Args...> args) {
    CallInternal<R>(f, absl::make_index_sequence<sizeof...(Args)>{}, std::move(args));
    return PackVoid();
  }

  template <typename R, typename F, typename... Args>
  static absl::enable_if_t<!std::is_void<R>::value, msgpack::sbuffer> Call(
      const F &f, std::tuple<Args...> args) {
    auto r =
        CallInternal<R>(f, absl::make_index_sequence<sizeof...(Args)>{}, std::move(args));
    return PackReturnValue(r);
  }

  template <typename R, typename F, size_t... I, typename... Args>
  static R CallInternal(const F &f, const absl::index_sequence<I...> &,
                        std::tuple<Args...> args) {
    (void)args;
    using ArgsTuple = boost::callable_traits::args_t<F>;
    return f(((typename std::tuple_element<I, ArgsTuple>::type)std::get<I>(args))...);
  }

  template <typename R, typename F, typename Self, typename... Args>
  static absl::enable_if_t<std::is_void<R>::value, msgpack::sbuffer> CallMember(
      const F &f, Self *self, std::tuple<Args...> args) {
    CallMemberInternal<R>(f, self, absl::make_index_sequence<sizeof...(Args)>{},
                          std::move(args));
    return PackVoid();
  }

  template <typename R, typename F, typename Self, typename... Args>
  static absl::enable_if_t<!std::is_void<R>::value, msgpack::sbuffer> CallMember(
      const F &f, Self *self, std::tuple<Args...> args) {
    auto r = CallMemberInternal<R>(f, self, absl::make_index_sequence<sizeof...(Args)>{},
                                   std::move(args));
    return PackReturnValue(r);
  }

  template <typename R, typename F, typename Self, size_t... I, typename... Args>
  static R CallMemberInternal(const F &f, Self *self, const absl::index_sequence<I...> &,
                              std::tuple<Args...> args) {
    (void)args;
    using ArgsTuple = boost::callable_traits::args_t<F>;
    return (self->*f)(
        ((typename std::tuple_element<I + 1, ArgsTuple>::type) std::get<I>(args))...);
  }
};

/// Manage all ray remote functions, add remote functions by RAY_REMOTE, get functions by
/// TaskExecutionHandler.
class FunctionManager {
 public:
  static FunctionManager &Instance() {
    static FunctionManager instance;
    return instance;
  }

  std::function<msgpack::sbuffer(const std::vector<msgpack::sbuffer> &)> *GetFunction(
      const std::string &func_name) {
    auto it = map_invokers_.find(func_name);
    if (it == map_invokers_.end()) {
      return nullptr;
    }

    return &it->second;
  }

  template <typename Function>
  absl::enable_if_t<!std::is_member_function_pointer<Function>::value, bool>
  RegisterRemoteFunction(std::string const &name, const Function &f) {
    auto pair = func_ptr_to_key_map_.emplace(GetAddress(f), name);
    if (!pair.second) {
      throw ray::api::RayException("Duplicate RAY_REMOTE function: " + name);
    }

    bool ok = RegisterNonMemberFunc(name, f);
    if (!ok) {
      throw ray::api::RayException("Duplicate RAY_REMOTE function: " + name);
    }

    return true;
  }

  template <typename Function>
  absl::enable_if_t<std::is_member_function_pointer<Function>::value, bool>
  RegisterRemoteFunction(std::string const &name, const Function &f) {
    using Self = boost::callable_traits::class_of_t<Function>;
    auto key = std::make_pair(typeid(Self).name(), GetAddress(f));
    auto pair = mem_func_to_key_map_.emplace(std::move(key), name);
    if (!pair.second) {
      throw ray::api::RayException("Duplicate RAY_REMOTE function: " + name);
    }

    bool ok = RegisterMemberFunc(name, f);
    if (!ok) {
      throw ray::api::RayException("Duplicate RAY_REMOTE function: " + name);
    }

    return true;
  }

  template <typename Function>
  absl::enable_if_t<!std::is_member_function_pointer<Function>::value, std::string>
  GetFunctionName(const Function &f) {
    auto it = func_ptr_to_key_map_.find(GetAddress(f));
    if (it == func_ptr_to_key_map_.end()) {
      return "";
    }

    return it->second;
  }

  template <typename Function>
  absl::enable_if_t<std::is_member_function_pointer<Function>::value, std::string>
  GetFunctionName(const Function &f) {
    using Self = boost::callable_traits::class_of_t<Function>;
    auto key = std::make_pair(typeid(Self).name(), GetAddress(f));
    auto it = mem_func_to_key_map_.find(key);
    if (it == mem_func_to_key_map_.end()) {
      return "";
    }

    return it->second;
  }

  std::function<msgpack::sbuffer(msgpack::sbuffer *,
                                 const std::vector<msgpack::sbuffer> &)>
      *GetMemberFunction(const std::string &func_name) {
    auto it = map_mem_func_invokers_.find(func_name);
    if (it == map_mem_func_invokers_.end()) {
      return nullptr;
    }

    return &it->second;
  }

 private:
  FunctionManager() = default;
  ~FunctionManager() = default;
  FunctionManager(const FunctionManager &) = delete;
  FunctionManager(FunctionManager &&) = delete;

  template <typename Function>
  bool RegisterNonMemberFunc(std::string const &name, Function f) {
    return map_invokers_
        .emplace(name, std::bind(&Invoker<Function>::Apply, std::move(f),
                                 std::placeholders::_1))
        .second;
  }

  template <typename Function>
  bool RegisterMemberFunc(std::string const &name, Function f) {
    return map_mem_func_invokers_
        .emplace(name, std::bind(&Invoker<Function>::ApplyMember, std::move(f),
                                 std::placeholders::_1, std::placeholders::_2))
        .second;
  }

  template <class Dest, class Source>
  Dest BitCast(const Source &source) {
    static_assert(sizeof(Dest) == sizeof(Source),
                  "BitCast requires source and destination to be the same size");

    Dest dest;
    memcpy(&dest, &source, sizeof(dest));
    return dest;
  }

  template <typename F>
  std::string GetAddress(F f) {
    auto arr = BitCast<std::array<char, sizeof(F)>>(f);
    return std::string(arr.data(), arr.size());
  }

  std::unordered_map<
      std::string, std::function<msgpack::sbuffer(const std::vector<msgpack::sbuffer> &)>>
      map_invokers_;
  std::unordered_map<std::string,
                     std::function<msgpack::sbuffer(
                         msgpack::sbuffer *, const std::vector<msgpack::sbuffer> &)>>
      map_mem_func_invokers_;
  std::unordered_map<std::string, std::string> func_ptr_to_key_map_;
  std::map<std::pair<std::string, std::string>, std::string> mem_func_to_key_map_;
};
}  // namespace internal
}  // namespace ray