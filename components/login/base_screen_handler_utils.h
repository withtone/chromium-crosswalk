// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_LOGIN_BASE_SCREEN_HANDLER_UTILS_H_
#define COMPONENTS_LOGIN_BASE_SCREEN_HANDLER_UTILS_H_

#include <cstddef>
#include <string>

#include "base/callback.h"
#include "base/logging.h"
#include "base/strings/string16.h"
#include "base/tuple.h"
#include "base/values.h"
#include "components/login/login_export.h"

namespace login {

typedef std::vector<std::string> StringList;
typedef std::vector<base::string16> String16List;

template <typename T>
struct LOGIN_EXPORT UnwrapConstRef {
  typedef T Type;
};

template <typename T>
struct LOGIN_EXPORT UnwrapConstRef<const T&> {
  typedef T Type;
};

bool LOGIN_EXPORT ParseValue(const base::Value* value, bool* out_value);
bool LOGIN_EXPORT ParseValue(const base::Value* value, int* out_value);
bool LOGIN_EXPORT ParseValue(const base::Value* value, double* out_value);
bool LOGIN_EXPORT ParseValue(const base::Value* value, std::string* out_value);
bool LOGIN_EXPORT
    ParseValue(const base::Value* value, base::string16* out_value);
bool LOGIN_EXPORT ParseValue(const base::Value* value,
                             const base::DictionaryValue** out_value);
bool LOGIN_EXPORT ParseValue(const base::Value* value, StringList* out_value);
bool LOGIN_EXPORT ParseValue(const base::Value* value, String16List* out_value);

template <typename T>
inline bool GetArg(const base::ListValue* args, size_t index, T* out_value) {
  const base::Value* value;
  if (!args->Get(index, &value))
    return false;
  return ParseValue(value, out_value);
}

base::FundamentalValue LOGIN_EXPORT MakeValue(bool v);
base::FundamentalValue LOGIN_EXPORT MakeValue(int v);
base::FundamentalValue LOGIN_EXPORT MakeValue(double v);
base::StringValue LOGIN_EXPORT MakeValue(const std::string& v);
base::StringValue LOGIN_EXPORT MakeValue(const base::string16& v);

template <typename T>
inline const T& MakeValue(const T& v) {
  return v;
}

template <typename Arg, size_t index>
typename UnwrapConstRef<Arg>::Type ParseArg(const base::ListValue* args) {
  typename UnwrapConstRef<Arg>::Type parsed;
  CHECK(GetArg(args, index, &parsed));
  return parsed;
}

template <typename... Args, size_t... Ns>
inline void DispatchToCallback(const base::Callback<void(Args...)>& callback,
                               const base::ListValue* args,
                               IndexSequence<Ns...> indexes) {
  DCHECK(args);
  DCHECK_EQ(sizeof...(Args), args->GetSize());

  callback.Run(ParseArg<Args, Ns>(args)...);
}

template <typename... Args>
void CallbackWrapper(const base::Callback<void(Args...)>& callback,
                     const base::ListValue* args) {
  DispatchToCallback(callback, args, MakeIndexSequence<sizeof...(Args)>());
}


}  // namespace login

#endif  // COMPONENTS_LOGIN_BASE_SCREEN_HANDLER_UTILS_H_
