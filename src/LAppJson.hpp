/**
 * @brief Small JSON reader/writer helpers for the IPC protocol.
 *
 * Supports the full JSON grammar (objects, arrays, strings with \uXXXX
 * escapes incl. surrogate pairs, numbers, true/false/null). Objects keep
 * their key order.
 */

#pragma once

#include <string>
#include <vector>

class JsonValue
{
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : _type(Null), _bool(false), _number(0.0) {}

    /** Parse `text`. On failure returns false and sets `error`. */
    static bool Parse(const std::string& text, JsonValue& out, std::string& error);

    Type GetType() const { return _type; }
    bool IsNull() const { return _type == Null; }
    bool IsBool() const { return _type == Bool; }
    bool IsNumber() const { return _type == Number; }
    bool IsString() const { return _type == String; }
    bool IsArray() const { return _type == Array; }
    bool IsObject() const { return _type == Object; }

    bool AsBool() const { return _bool; }
    double AsNumber() const { return _number; }
    const std::string& AsString() const { return _string; }

    /** Array items, or object values (aligned with Keys()). */
    const std::vector<JsonValue>& Items() const { return _items; }
    /** Object keys, in document order. */
    const std::vector<std::string>& Keys() const { return _keys; }

    /** Object member lookup; NULL if missing or not an object. */
    const JsonValue* Find(const std::string& key) const;

private:
    friend class JsonParser;
    Type _type;
    bool _bool;
    double _number;
    std::string _string;
    std::vector<std::string> _keys;
    std::vector<JsonValue> _items;
};

/** JSON string literal (with quotes) for `s` (UTF-8 passed through). */
std::string JsonQuote(const std::string& s);

/** JSON number literal; non-finite values become null. */
std::string JsonNumber(double v);

inline std::string JsonBool(bool b) { return b ? "true" : "false"; }

/** JSON array of string literals. */
std::string JsonStringArray(const std::vector<std::string>& items);

/**
 * Incrementally builds a JSON object: JsonObjectBuilder().Add("a", JsonNumber(1)).Str()
 * Values passed to Add() must already be JSON text.
 */
class JsonObjectBuilder
{
public:
    JsonObjectBuilder() : _out("{"), _first(true) {}
    JsonObjectBuilder& Add(const std::string& key, const std::string& rawJson)
    {
        if (!_first) _out += ",";
        _first = false;
        _out += JsonQuote(key);
        _out += ":";
        _out += rawJson;
        return *this;
    }
    std::string Str() const { return _out + "}"; }

private:
    std::string _out;
    bool _first;
};
