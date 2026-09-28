// A small TOML reader, covering the subset the game manifests use
// (AMIGA_RECOMP.md 5). The project vendors no dependencies, and a manifest
// format nobody can parse without a library is a worse trade than 250 lines.
//
// Supported: comments, [tables], [[arrays of tables]], dotted table names,
// string / integer (decimal, 0x hex, $ hex) / boolean / array values.
// Anything else is a parse error with a line number, never a silent default.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace arecomp::toml {

struct Value {
    enum class Type { String, Integer, Boolean, Array };

    Type type = Type::String;
    std::string string;
    std::int64_t integer = 0;
    bool boolean = false;
    std::vector<Value> array;
};

class Table {
public:
    [[nodiscard]] bool has(const std::string& key) const;
    [[nodiscard]] const Value* find(const std::string& key) const;

    // Typed accessors. Each reports a missing or wrong-typed key through
    // `error`, so a manifest mistake is a diagnostic rather than a default.
    [[nodiscard]] std::string string(const std::string& key,
                                     const std::string& fallback = {}) const;
    [[nodiscard]] std::uint32_t integer(const std::string& key,
                                        std::uint32_t fallback = 0) const;
    [[nodiscard]] bool boolean(const std::string& key, bool fallback = false) const;
    [[nodiscard]] std::vector<std::uint32_t> integers(const std::string& key) const;
    [[nodiscard]] std::vector<std::string> strings(const std::string& key) const;

    void set(const std::string& key, Value value) { values_[key] = std::move(value); }
    [[nodiscard]] const std::map<std::string, Value>& values() const { return values_; }

private:
    std::map<std::string, Value> values_;
};

class Document {
public:
    // Returns false and fills `error` on a malformed document.
    bool parse(const std::string& text, std::string& error);
    bool parse_file(const std::string& path, std::string& error);

    // A named table, or an empty one if absent.
    [[nodiscard]] const Table& table(const std::string& name) const;
    [[nodiscard]] bool has_table(const std::string& name) const;
    // Entries of an [[array of tables]].
    [[nodiscard]] const std::vector<Table>& array(const std::string& name) const;

private:
    std::map<std::string, Table> tables_;
    std::map<std::string, std::vector<Table>> arrays_;
};

} // namespace arecomp::toml
