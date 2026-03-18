#include "bencode.h"
#include <stdexcept>
#include <sstream>
#include <cctype>

namespace bencode {

namespace {

// Internal parse function with position tracking
BencodeValue parseImpl(const std::string& data, size_t& pos) {
    if (pos >= data.size()) {
        throw std::runtime_error("Unexpected end of bencode data");
    }

    char c = data[pos];

    // Integer: i<number>e
    if (c == 'i') {
        pos++;  // skip 'i'
        size_t end = data.find('e', pos);
        if (end == std::string::npos) {
            throw std::runtime_error("Unterminated integer");
        }
        std::string numStr = data.substr(pos, end - pos);
        pos = end + 1;  // skip 'e'
        return BencodeInt(std::stoll(numStr));
    }

    // String: <length>:<data>
    if (std::isdigit(static_cast<unsigned char>(c))) {
        size_t colon = data.find(':', pos);
        if (colon == std::string::npos) {
            throw std::runtime_error("Missing colon in string");
        }
        size_t length = std::stoull(data.substr(pos, colon - pos));
        pos = colon + 1;
        if (pos + length > data.size()) {
            throw std::runtime_error("String length exceeds data");
        }
        std::string str = data.substr(pos, length);
        pos += length;
        return BencodeString(std::move(str));
    }

    // List: l<values>e
    if (c == 'l') {
        pos++;  // skip 'l'
        BencodeList list;
        while (pos < data.size() && data[pos] != 'e') {
            list.push_back(parseImpl(data, pos));
        }
        if (pos >= data.size()) {
            throw std::runtime_error("Unterminated list");
        }
        pos++;  // skip 'e'
        return BencodeValue(std::move(list));
    }

    // Dictionary: d<key><value>...e
    if (c == 'd') {
        pos++;  // skip 'd'
        BencodeDict dict;
        while (pos < data.size() && data[pos] != 'e') {
            // Key must be a string
            BencodeValue key = parseImpl(data, pos);
            if (!key.isString()) {
                throw std::runtime_error("Dictionary key must be a string");
            }
            BencodeValue value = parseImpl(data, pos);
            dict[std::move(key.asString())] = std::move(value);
        }
        if (pos >= data.size()) {
            throw std::runtime_error("Unterminated dictionary");
        }
        pos++;  // skip 'e'
        return BencodeValue(std::move(dict));
    }

    throw std::runtime_error(std::string("Invalid bencode character: ") + c);
}

} // anonymous namespace

BencodeValue parse(const std::string& data, size_t& pos) {
    return parseImpl(data, pos);
}

BencodeValue parse(const std::string& data) {
    size_t pos = 0;
    return parseImpl(data, pos);
}

std::string encode(const BencodeValue& value) {
    std::ostringstream oss;

    if (value.isInt()) {
        oss << 'i' << value.asInt() << 'e';
    }
    else if (value.isString()) {
        const auto& str = value.asString();
        oss << str.size() << ':' << str;
    }
    else if (value.isList()) {
        oss << 'l';
        for (const auto& item : value.asList()) {
            oss << encode(item);
        }
        oss << 'e';
    }
    else if (value.isDict()) {
        oss << 'd';
        for (const auto& [key, val] : value.asDict()) {
            oss << key.size() << ':' << key;
            oss << encode(val);
        }
        oss << 'e';
    }

    return oss.str();
}

BencodeValue* dictGet(BencodeDict& dict, const std::string& key) {
    auto it = dict.find(key);
    return it != dict.end() ? &it->second : nullptr;
}

const BencodeValue* dictGet(const BencodeDict& dict, const std::string& key) {
    auto it = dict.find(key);
    return it != dict.end() ? &it->second : nullptr;
}

std::optional<BencodeInt> dictGetInt(const BencodeDict& dict, const std::string& key) {
    const BencodeValue* val = dictGet(dict, key);
    if (val && val->isInt()) {
        return val->asInt();
    }
    return std::nullopt;
}

std::optional<BencodeString> dictGetString(const BencodeDict& dict, const std::string& key) {
    const BencodeValue* val = dictGet(dict, key);
    if (val && val->isString()) {
        return val->asString();
    }
    return std::nullopt;
}

} // namespace bencode
