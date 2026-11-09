// lobby_json.h -- the little JSON the online lobby client needs: parse a
// server message into a tree, build an op with escaped strings.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mpnet {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    // Null on malformed input.
    static Json Parse(const std::string& text);

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsObject() const { return type_ == Type::Object; }
    bool IsArray() const { return type_ == Type::Array; }

    // Member of an object; a shared null value when absent.
    const Json& operator[](const std::string& key) const;
    // Element of an array; a shared null value when out of range.
    const Json& operator[](size_t index) const;
    size_t size() const;

    std::string Str(const std::string& def = "") const;
    double Num(double def = 0) const;
    int Int(int def = 0) const;
    bool Bool(bool def = false) const;

    // Shorthand for (*this)[key].Str() and so on.
    std::string Str(const std::string& key, const std::string& def) const {
        return (*this)[key].Str(def);
    }
    int Int(const std::string& key, int def) const {
        return (*this)[key].Int(def);
    }
    bool Bool(const std::string& key, bool def) const {
        return (*this)[key].Bool(def);
    }

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> items_;
    std::map<std::string, Json> members_;
};

// `text` as a JSON string literal, quotes included.
std::string JsonQuote(const std::string& text);

// Builds one flat object: JsonOp("join").Str("lobby_id", id).Text().
class JsonOp {
public:
    explicit JsonOp(const std::string& op);
    JsonOp& Str(const std::string& key, const std::string& value);
    JsonOp& Int(const std::string& key, int64_t value);
    JsonOp& Bool(const std::string& key, bool value);
    std::string Text() const { return body_ + "}"; }

private:
    std::string body_;
};

}  // namespace mpnet
