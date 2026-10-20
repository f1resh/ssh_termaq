#pragma once
#include <nlohmann/json.hpp>

namespace st {
using Json = nlohmann::json;

/** Deep merge `patch` into `base` (objects merge recursively; arrays/primitives replace; null values in patch are ignored). */
inline void jsonDeepMerge(Json& base, const Json& patch) {
    if (!patch.is_object()) {
        if (!patch.is_null()) base = patch;
        return;
    }
    if (!base.is_object()) base = Json::object();
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.value().is_null()) continue;
        if (it.value().is_object() && base.contains(it.key()) && base[it.key()].is_object())
            jsonDeepMerge(base[it.key()], it.value());
        else
            base[it.key()] = it.value();
    }
}

/** Safe accessors for positional bridge arguments (args is a JSON array). */
inline const Json& jsonArg(const Json& args, size_t i) {
    static const Json kNull;
    return (args.is_array() && i < args.size()) ? args[i] : kNull;
}
inline std::string argStr(const Json& args, size_t i, const std::string& def = {}) {
    const Json& v = jsonArg(args, i);
    return v.is_string() ? v.get<std::string>() : def;
}
inline int64_t argInt(const Json& args, size_t i, int64_t def = 0) {
    const Json& v = jsonArg(args, i);
    return v.is_number() ? v.get<int64_t>() : def;
}
inline double argNum(const Json& args, size_t i, double def = 0) {
    const Json& v = jsonArg(args, i);
    return v.is_number() ? v.get<double>() : def;
}
inline bool argBool(const Json& args, size_t i, bool def = false) {
    const Json& v = jsonArg(args, i);
    return v.is_boolean() ? v.get<bool>() : def;
}
inline std::vector<std::string> argStrList(const Json& args, size_t i) {
    std::vector<std::string> out;
    const Json& v = jsonArg(args, i);
    if (v.is_array())
        for (const auto& e : v)
            if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}
}  // namespace st
