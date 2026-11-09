// lobby_json.cpp -- see lobby_json.h.

#include "net/lobby_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mpnet {

namespace {
const Json& NullJson() {
    static const Json null;
    return null;
}
constexpr int kMaxDepth = 32;
}  // namespace

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool ParseDocument(Json* out) {
        if (!Value(out, 0)) return false;
        Space();
        return i_ == s_.size();
    }

private:
    void Space() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' ||
                s_[i_] == '\r'))
            ++i_;
    }

    bool Literal(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }

    static void PutUtf8(uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool Hex4(uint32_t* out) {
        if (i_ + 4 > s_.size()) return false;
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        *out = v;
        return true;
    }

    bool String(std::string* out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(&cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i_ + 1 < s_.size() &&
                    s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                    i_ += 2;
                    uint32_t lo;
                    if (!Hex4(&lo)) return false;
                    if (lo >= 0xDC00 && lo < 0xE000)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                PutUtf8(cp, out);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool Value(Json* out, int depth) {
        if (depth > kMaxDepth) return false;
        Space();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            out->type_ = Json::Type::Object;
            Space();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            for (;;) {
                Space();
                std::string key;
                if (!String(&key)) return false;
                Space();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                ++i_;
                Json value;
                if (!Value(&value, depth + 1)) return false;
                out->members_[key] = std::move(value);
                Space();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            out->type_ = Json::Type::Array;
            Space();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            for (;;) {
                Json value;
                if (!Value(&value, depth + 1)) return false;
                out->items_.push_back(std::move(value));
                Space();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out->type_ = Json::Type::String;
            return String(&out->str_);
        }
        if (Literal("true")) {
            out->type_ = Json::Type::Bool;
            out->bool_ = true;
            return true;
        }
        if (Literal("false")) {
            out->type_ = Json::Type::Bool;
            return true;
        }
        if (Literal("null")) return true;
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* begin = s_.c_str() + i_;
            char* end = nullptr;
            const double v = std::strtod(begin, &end);
            if (end == begin) return false;
            i_ += static_cast<size_t>(end - begin);
            out->type_ = Json::Type::Number;
            out->num_ = v;
            return true;
        }
        return false;
    }

    const std::string& s_;
    size_t i_ = 0;
};

Json Json::Parse(const std::string& text) {
    Json out;
    JsonParser parser(text);
    if (!parser.ParseDocument(&out)) return Json();
    return out;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return NullJson();
    auto it = members_.find(key);
    return it == members_.end() ? NullJson() : it->second;
}

const Json& Json::operator[](size_t index) const {
    if (type_ != Type::Array || index >= items_.size()) return NullJson();
    return items_[index];
}

size_t Json::size() const {
    if (type_ == Type::Array) return items_.size();
    if (type_ == Type::Object) return members_.size();
    return 0;
}

std::string Json::Str(const std::string& def) const {
    return type_ == Type::String ? str_ : def;
}

double Json::Num(double def) const {
    return type_ == Type::Number ? num_ : def;
}

int Json::Int(int def) const {
    if (type_ != Type::Number || !std::isfinite(num_)) return def;
    if (num_ > 2147483647.0 || num_ < -2147483648.0) return def;
    return static_cast<int>(num_);
}

bool Json::Bool(bool def) const {
    return type_ == Type::Bool ? bool_ : def;
}

std::string JsonQuote(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                out += esc;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out += "\"";
    return out;
}

JsonOp::JsonOp(const std::string& op) : body_("{\"op\":" + JsonQuote(op)) {}

JsonOp& JsonOp::Str(const std::string& key, const std::string& value) {
    body_ += "," + JsonQuote(key) + ":" + JsonQuote(value);
    return *this;
}

JsonOp& JsonOp::Int(const std::string& key, int64_t value) {
    body_ += "," + JsonQuote(key) + ":" + std::to_string(value);
    return *this;
}

JsonOp& JsonOp::Bool(const std::string& key, bool value) {
    body_ += "," + JsonQuote(key) + ":" + (value ? "true" : "false");
    return *this;
}

}  // namespace mpnet
