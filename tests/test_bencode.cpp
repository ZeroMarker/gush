#include <gtest/gtest.h>
#include "bencode.h"
#include <stdexcept>

// Test integer parsing
TEST(BencodeTest, ParseInteger) {
    bencode::BencodeValue val = bencode::parse("i42e");
    EXPECT_TRUE(val.isInt());
    EXPECT_EQ(val.asInt(), 42);
}

TEST(BencodeTest, ParseNegativeInteger) {
    bencode::BencodeValue val = bencode::parse("i-3e");
    EXPECT_TRUE(val.isInt());
    EXPECT_EQ(val.asInt(), -3);
}

TEST(BencodeTest, ParseZero) {
    bencode::BencodeValue val = bencode::parse("i0e");
    EXPECT_TRUE(val.isInt());
    EXPECT_EQ(val.asInt(), 0);
}

TEST(BencodeTest, ParseIntegerFail) {
    EXPECT_THROW(bencode::parse("i42"), std::runtime_error);
    EXPECT_THROW(bencode::parse("i"), std::runtime_error);
}

// Test string parsing
TEST(BencodeTest, ParseString) {
    bencode::BencodeValue val = bencode::parse("4:spam");
    EXPECT_TRUE(val.isString());
    EXPECT_EQ(val.asString(), "spam");
}

TEST(BencodeTest, ParseEmptyString) {
    bencode::BencodeValue val = bencode::parse("0:");
    EXPECT_TRUE(val.isString());
    EXPECT_EQ(val.asString(), "");
}

TEST(BencodeTest, ParseStringWithSpecialChars) {
    bencode::BencodeValue val = bencode::parse("11:hello world");
    EXPECT_TRUE(val.isString());
    EXPECT_EQ(val.asString(), "hello world");
}

// Test list parsing
TEST(BencodeTest, ParseList) {
    bencode::BencodeValue val = bencode::parse("l4:spami42ee");
    EXPECT_TRUE(val.isList());
    const auto& list = val.asList();
    EXPECT_EQ(list.size(), 2u);
    EXPECT_TRUE(list[0].isString());
    EXPECT_EQ(list[0].asString(), "spam");
    EXPECT_TRUE(list[1].isInt());
    EXPECT_EQ(list[1].asInt(), 42);
}

TEST(BencodeTest, ParseEmptyList) {
    bencode::BencodeValue val = bencode::parse("le");
    EXPECT_TRUE(val.isList());
    EXPECT_EQ(val.asList().size(), 0u);
}

TEST(BencodeTest, ParseNestedList) {
    bencode::BencodeValue val = bencode::parse("ll4:spamee");
    EXPECT_TRUE(val.isList());
    const auto& outerList = val.asList();
    EXPECT_EQ(outerList.size(), 1u);
    EXPECT_TRUE(outerList[0].isList());
    const auto& innerList = outerList[0].asList();
    EXPECT_EQ(innerList.size(), 1u);
    EXPECT_EQ(innerList[0].asString(), "spam");
}

// Test dictionary parsing
TEST(BencodeTest, ParseDict) {
    bencode::BencodeValue val = bencode::parse("d3:bari20e3:fooi42ee");
    EXPECT_TRUE(val.isDict());
    const auto& dict = val.asDict();
    EXPECT_EQ(dict.size(), 2u);
    
    const auto* bar = bencode::dictGet(dict, "bar");
    ASSERT_NE(bar, nullptr);
    EXPECT_TRUE(bar->isInt());
    EXPECT_EQ(bar->asInt(), 20);
    
    const auto* foo = bencode::dictGet(dict, "foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_TRUE(foo->isInt());
    EXPECT_EQ(foo->asInt(), 42);
}

TEST(BencodeTest, ParseEmptyDict) {
    bencode::BencodeValue val = bencode::parse("de");
    EXPECT_TRUE(val.isDict());
    EXPECT_EQ(val.asDict().size(), 0u);
}

TEST(BencodeTest, ParseNestedDict) {
    bencode::BencodeValue val = bencode::parse("d1:xd1:yi10eee");
    EXPECT_TRUE(val.isDict());
    const auto& outerDict = val.asDict();
    const auto* x = bencode::dictGet(outerDict, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(x->isDict());
    const auto& innerDict = x->asDict();
    const auto* y = bencode::dictGet(innerDict, "y");
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->asInt(), 10);
}

TEST(BencodeTest, DictGetInt) {
    bencode::BencodeValue val = bencode::parse("d3:numi100ee");
    const auto& dict = val.asDict();
    
    auto result = bencode::dictGetInt(dict, "num");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 100);
    
    auto missing = bencode::dictGetInt(dict, "missing");
    EXPECT_FALSE(missing.has_value());
}

TEST(BencodeTest, DictGetString) {
    bencode::BencodeValue val = bencode::parse("d4:name4:testee");
    const auto& dict = val.asDict();
    
    auto result = bencode::dictGetString(dict, "name");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "test");
    
    auto missing = bencode::dictGetString(dict, "missing");
    EXPECT_FALSE(missing.has_value());
}

// Test encoding
TEST(BencodeTest, EncodeInteger) {
    bencode::BencodeValue val(42);
    EXPECT_EQ(bencode::encode(val), "i42e");
}

TEST(BencodeTest, EncodeNegativeInteger) {
    bencode::BencodeValue val(-3);
    EXPECT_EQ(bencode::encode(val), "i-3e");
}

TEST(BencodeTest, EncodeString) {
    bencode::BencodeValue val(std::string("spam"));
    EXPECT_EQ(bencode::encode(val), "4:spam");
}

TEST(BencodeTest, EncodeEmptyString) {
    bencode::BencodeValue val(std::string(""));
    EXPECT_EQ(bencode::encode(val), "0:");
}

TEST(BencodeTest, EncodeList) {
    bencode::BencodeList list;
    list.push_back(bencode::BencodeString("spam"));
    list.push_back(bencode::BencodeInt(42));
    bencode::BencodeValue val(std::move(list));
    EXPECT_EQ(bencode::encode(val), "l4:spami42ee");
}

TEST(BencodeTest, EncodeDict) {
    bencode::BencodeDict dict;
    dict["foo"] = bencode::BencodeInt(42);
    dict["bar"] = bencode::BencodeInt(20);
    bencode::BencodeValue val(std::move(dict));
    // Keys are sorted alphabetically
    EXPECT_EQ(bencode::encode(val), "d3:bari20e3:fooi42ee");
}

// Test round-trip
TEST(BencodeTest, RoundTripInteger) {
    std::string original = "i12345e";
    bencode::BencodeValue val = bencode::parse(original);
    EXPECT_EQ(bencode::encode(val), original);
}

TEST(BencodeTest, RoundTripString) {
    std::string original = "5:hello";
    bencode::BencodeValue val = bencode::parse(original);
    EXPECT_EQ(bencode::encode(val), original);
}

TEST(BencodeTest, RoundTripComplex) {
    std::string original = "d4:infod6:lengthi1000e4:name4:testee";
    bencode::BencodeValue val = bencode::parse(original);
    EXPECT_EQ(bencode::encode(val), original);
}

// Test type checking
TEST(BencodeTest, TypeChecking) {
    bencode::BencodeValue intVal(42);
    EXPECT_TRUE(intVal.isInt());
    EXPECT_FALSE(intVal.isString());
    EXPECT_FALSE(intVal.isList());
    EXPECT_FALSE(intVal.isDict());
    
    bencode::BencodeValue strVal(std::string("test"));
    EXPECT_FALSE(strVal.isInt());
    EXPECT_TRUE(strVal.isString());
    EXPECT_FALSE(strVal.isList());
    EXPECT_FALSE(strVal.isDict());
}

// Test default values
TEST(BencodeTest, DefaultValues) {
    bencode::BencodeValue intVal(42);
    EXPECT_EQ(intVal.asInt(0), 42);
    
    bencode::BencodeValue strVal(std::string("test"));
    EXPECT_EQ(strVal.asStringDef("default"), "test");
    
    bencode::BencodeValue wrongType(42);
    EXPECT_EQ(wrongType.asStringDef("default"), "default");
}
