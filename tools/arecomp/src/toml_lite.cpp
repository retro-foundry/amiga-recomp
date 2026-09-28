#include "toml_lite.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace arecomp::toml {

namespace {

const Table& empty_table() {
    static const Table table;
    return table;
}

const std::vector<Table>& empty_array() {
    static const std::vector<Table> array;
    return array;
}

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Strips a comment, respecting quoted strings so a '#' inside a path survives.
std::string strip_comment(const std::string& line) {
    bool in_string = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') in_string = !in_string;
        else if (line[i] == '#' && !in_string) return line.substr(0, i);
    }
    return line;
}

bool parse_integer(const std::string& text, std::int64_t& out) {
    std::string s = trim(text);
    if (s.empty()) return false;
    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        s.erase(0, 1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.erase(0, 2);
    } else if (!s.empty() && s[0] == '$') {   // Amiga convention, accepted too
        base = 16;
        s.erase(0, 1);
    }
    // Underscores as digit separators.
    std::string digits;
    for (char c : s) {
        if (c != '_') digits += c;
    }
    if (digits.empty()) return false;

    char* end = nullptr;
    const auto value = std::strtoll(digits.c_str(), &end, base);
    if (end == nullptr || *end != '\0') return false;
    out = negative ? -value : value;
    return true;
}

bool parse_value(const std::string& text, Value& out, std::string& error);

bool parse_array(const std::string& text, Value& out, std::string& error) {
    out.type = Value::Type::Array;
    const std::string inner = trim(text.substr(1, text.size() - 2));
    if (inner.empty()) return true;

    std::string current;
    int depth = 0;
    bool in_string = false;
    for (char c : inner) {
        if (c == '"') in_string = !in_string;
        if (!in_string) {
            if (c == '[') ++depth;
            if (c == ']') --depth;
            if (c == ',' && depth == 0) {
                Value element;
                if (!parse_value(trim(current), element, error)) return false;
                out.array.push_back(std::move(element));
                current.clear();
                continue;
            }
        }
        current += c;
    }
    if (!trim(current).empty()) {
        Value element;
        if (!parse_value(trim(current), element, error)) return false;
        out.array.push_back(std::move(element));
    }
    return true;
}

bool parse_value(const std::string& text, Value& out, std::string& error) {
    const std::string s = trim(text);
    if (s.empty()) {
        error = "empty value";
        return false;
    }
    if (s.front() == '"' && s.back() == '"' && s.size() >= 2) {
        out.type = Value::Type::String;
        out.string.clear();
        for (std::size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size()) {
                ++i;
                switch (s[i]) {
                case 'n': out.string += '\n'; break;
                case 't': out.string += '\t'; break;
                case '\\': out.string += '\\'; break;
                case '"': out.string += '"'; break;
                default: out.string += s[i]; break;
                }
            } else {
                out.string += s[i];
            }
        }
        return true;
    }
    if (s.front() == '[' && s.back() == ']') return parse_array(s, out, error);
    if (s == "true" || s == "false") {
        out.type = Value::Type::Boolean;
        out.boolean = s == "true";
        return true;
    }
    std::int64_t integer = 0;
    if (parse_integer(s, integer)) {
        out.type = Value::Type::Integer;
        out.integer = integer;
        return true;
    }
    error = "unrecognised value: " + s;
    return false;
}

} // namespace

bool Table::has(const std::string& key) const { return values_.count(key) != 0; }

const Value* Table::find(const std::string& key) const {
    auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second;
}

std::string Table::string(const std::string& key, const std::string& fallback) const {
    const Value* v = find(key);
    return (v && v->type == Value::Type::String) ? v->string : fallback;
}

std::uint32_t Table::integer(const std::string& key, std::uint32_t fallback) const {
    const Value* v = find(key);
    return (v && v->type == Value::Type::Integer)
               ? static_cast<std::uint32_t>(v->integer)
               : fallback;
}

bool Table::boolean(const std::string& key, bool fallback) const {
    const Value* v = find(key);
    return (v && v->type == Value::Type::Boolean) ? v->boolean : fallback;
}

std::vector<std::uint32_t> Table::integers(const std::string& key) const {
    std::vector<std::uint32_t> out;
    const Value* v = find(key);
    if (!v || v->type != Value::Type::Array) return out;
    for (const Value& element : v->array) {
        if (element.type == Value::Type::Integer)
            out.push_back(static_cast<std::uint32_t>(element.integer));
    }
    return out;
}

std::vector<std::string> Table::strings(const std::string& key) const {
    std::vector<std::string> out;
    const Value* v = find(key);
    if (!v) return out;
    if (v->type == Value::Type::String) {
        out.push_back(v->string);
        return out;
    }
    if (v->type != Value::Type::Array) return out;
    for (const Value& element : v->array) {
        if (element.type == Value::Type::String) out.push_back(element.string);
    }
    return out;
}

bool Document::parse(const std::string& text, std::string& error) {
    std::istringstream stream(text);
    std::string line;
    int lineno = 0;

    Table* current = &tables_[""];   // keys before any header
    while (std::getline(stream, line)) {
        ++lineno;
        const std::string trimmed = trim(strip_comment(line));
        if (trimmed.empty()) continue;

        if (trimmed.size() > 4 && trimmed.substr(0, 2) == "[[" &&
            trimmed.substr(trimmed.size() - 2) == "]]") {
            const std::string name = trim(trimmed.substr(2, trimmed.size() - 4));
            arrays_[name].emplace_back();
            current = &arrays_[name].back();
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string name = trim(trimmed.substr(1, trimmed.size() - 2));
            current = &tables_[name];
            continue;
        }

        const auto equals = trimmed.find('=');
        if (equals == std::string::npos) {
            error = "line " + std::to_string(lineno) + ": expected key = value";
            return false;
        }
        const std::string key = trim(trimmed.substr(0, equals));
        std::string rest = trim(trimmed.substr(equals + 1));

        // An array may span lines. A manifest that records hundreds of
        // discovered entry points is unreadable on one line, so keep reading
        // until the brackets balance.
        if (!rest.empty() && rest.front() == '[') {
            int depth = 0;
            bool in_string = false;
            auto count = [&](const std::string& text) {
                for (char c : text) {
                    if (c == '"') in_string = !in_string;
                    else if (!in_string && c == '[') ++depth;
                    else if (!in_string && c == ']') --depth;
                }
            };
            count(rest);
            while (depth > 0 && std::getline(stream, line)) {
                ++lineno;
                const std::string more = trim(strip_comment(line));
                if (more.empty()) continue;
                rest += ' ';
                rest += more;
                count(more);
            }
            if (depth != 0) {
                error = "line " + std::to_string(lineno) + ": unterminated array";
                return false;
            }
        }
        if (key.empty()) {
            error = "line " + std::to_string(lineno) + ": empty key";
            return false;
        }

        Value value;
        std::string value_error;
        if (!parse_value(rest, value, value_error)) {
            error = "line " + std::to_string(lineno) + ": " + value_error;
            return false;
        }
        current->set(key, std::move(value));
    }
    return true;
}

bool Document::parse_file(const std::string& path, std::string& error) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::string text;
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0)
        text.append(buffer, n);
    std::fclose(file);
    return parse(text, error);
}

const Table& Document::table(const std::string& name) const {
    auto it = tables_.find(name);
    return it == tables_.end() ? empty_table() : it->second;
}

bool Document::has_table(const std::string& name) const {
    return tables_.count(name) != 0;
}

const std::vector<Table>& Document::array(const std::string& name) const {
    auto it = arrays_.find(name);
    return it == arrays_.end() ? empty_array() : it->second;
}

} // namespace arecomp::toml
