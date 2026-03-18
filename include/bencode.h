#ifndef BENCODE_H
#define BENCODE_H

#include <string>
#include <variant>
#include <vector>
#include <map>
#include <cstdint>
#include <optional>

namespace bencode {

// Forward declaration
struct BencodeValue;

using BencodeInt = int64_t;
using BencodeString = std::string;
using BencodeList = std::vector<BencodeValue>;
using BencodeDict = std::map<std::string, BencodeValue>;

// Bencode value type (recursive variant)
struct BencodeValue {
    std::variant<BencodeInt, BencodeString, BencodeList, BencodeDict> data;

    BencodeValue() = default;
    BencodeValue(BencodeInt i) : data(i) {}
    BencodeValue(const BencodeString& s) : data(s) {}
    BencodeValue(BencodeString&& s) : data(std::move(s)) {}
    BencodeValue(const char* s) : data(BencodeString(s)) {}
    BencodeValue(const BencodeList& l) : data(l) {}
    BencodeValue(BencodeList&& l) : data(std::move(l)) {}
    BencodeValue(const BencodeDict& d) : data(d) {}
    BencodeValue(BencodeDict&& d) : data(std::move(d)) {}

    // Copy constructor and assignment
    BencodeValue(const BencodeValue&) = default;
    BencodeValue& operator=(const BencodeValue&) = default;

    // Move constructor and assignment
    BencodeValue(BencodeValue&&) = default;
    BencodeValue& operator=(BencodeValue&&) = default;

    // Type checking
    bool isInt() const noexcept { return std::holds_alternative<BencodeInt>(data); }
    bool isString() const noexcept { return std::holds_alternative<BencodeString>(data); }
    bool isList() const noexcept { return std::holds_alternative<BencodeList>(data); }
    bool isDict() const noexcept { return std::holds_alternative<BencodeDict>(data); }

    // Type access (const versions)
    BencodeInt& asInt() { return std::get<BencodeInt>(data); }
    const BencodeInt& asInt() const { return std::get<BencodeInt>(data); }
    BencodeString& asString() { return std::get<BencodeString>(data); }
    const BencodeString& asString() const { return std::get<BencodeString>(data); }
    BencodeList& asList() { return std::get<BencodeList>(data); }
    const BencodeList& asList() const { return std::get<BencodeList>(data); }
    BencodeDict& asDict() { return std::get<BencodeDict>(data); }
    const BencodeDict& asDict() const { return std::get<BencodeDict>(data); }

    // Safe access with default
    BencodeInt asInt(BencodeInt def) const noexcept {
        return isInt() ? std::get<BencodeInt>(data) : def;
    }
    BencodeString asStringDef(const BencodeString& def = "") const {
        return isString() ? std::get<BencodeString>(data) : def;
    }
};

// Parse bencode data
BencodeValue parse(const std::string& data, size_t& pos);
BencodeValue parse(const std::string& data);

// Encode bencode value
std::string encode(const BencodeValue& value);

// Helper functions for dict access
BencodeValue* dictGet(BencodeDict& dict, const std::string& key);
const BencodeValue* dictGet(const BencodeDict& dict, const std::string& key);

// Safe dict get with type checking
std::optional<BencodeInt> dictGetInt(const BencodeDict& dict, const std::string& key);
std::optional<BencodeString> dictGetString(const BencodeDict& dict, const std::string& key);

} // namespace bencode

#endif // BENCODE_H
