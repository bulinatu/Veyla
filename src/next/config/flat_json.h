#pragma once

#include <map>
#include <string>
#include <string_view>

namespace brp::next {

// Minimal flat JSON object. The new configuration schema is deliberately flat:
// "section.key": value, which keeps migration and validation deterministic.
// Nested objects/arrays are skipped on read so the legacy session.json can be
// imported without a full DOM parser.
struct FlatValue {
    std::string text;
    bool quoted = false;
};

using FlatMap = std::map<std::string, FlatValue>;

class FlatJson {
public:
    static bool Parse(std::string_view input, FlatMap& out, std::string& error);
    static std::string Serialize(const FlatMap& values);

    static void PutString(FlatMap& map, const std::string& key, const std::string& value);
    static void PutInt(FlatMap& map, const std::string& key, long long value);
    static void PutDouble(FlatMap& map, const std::string& key, double value);
    static void PutBool(FlatMap& map, const std::string& key, bool value);

    static std::string GetString(const FlatMap& map, const std::string& key,
                                 const std::string& fallback);
    static long long GetInt(const FlatMap& map, const std::string& key, long long fallback);
    static double GetDouble(const FlatMap& map, const std::string& key, double fallback);
    static bool GetBool(const FlatMap& map, const std::string& key, bool fallback);
};

}  // namespace brp::next
