#include "LAppJson.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

class JsonParser
{
public:
    JsonParser(const std::string& text) : _s(text), _pos(0) {}

    bool ParseDocument(JsonValue& out, std::string& error)
    {
        SkipWs();
        if (!ParseValue(out, 0)) { error = _error; return false; }
        SkipWs();
        if (_pos != _s.size()) { error = Fail("unexpected trailing characters"); return false; }
        return true;
    }

private:
    static const int MaxDepth = 64;
    const std::string& _s;
    size_t _pos;
    std::string _error;

    std::string Fail(const std::string& msg)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), " at offset %zu", _pos);
        _error = msg + buf;
        return _error;
    }

    void SkipWs()
    {
        while (_pos < _s.size() && (_s[_pos] == ' ' || _s[_pos] == '\t' || _s[_pos] == '\n' || _s[_pos] == '\r')) _pos++;
    }

    bool Literal(const char* word)
    {
        size_t n = 0;
        while (word[n]) n++;
        if (_s.compare(_pos, n, word) != 0) return false;
        _pos += n;
        return true;
    }

    bool ParseValue(JsonValue& out, int depth)
    {
        if (depth > MaxDepth) { Fail("nesting too deep"); return false; }
        if (_pos >= _s.size()) { Fail("unexpected end of input"); return false; }
        char c = _s[_pos];
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') { out._type = JsonValue::String; return ParseString(out._string); }
        if (c == 't') { if (Literal("true")) { out._type = JsonValue::Bool; out._bool = true; return true; } }
        else if (c == 'f') { if (Literal("false")) { out._type = JsonValue::Bool; out._bool = false; return true; } }
        else if (c == 'n') { if (Literal("null")) { out._type = JsonValue::Null; return true; } }
        else if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        Fail("unexpected character");
        return false;
    }

    bool ParseNumber(JsonValue& out)
    {
        size_t start = _pos;
        if (_s[_pos] == '-') _pos++;
        while (_pos < _s.size() && ((_s[_pos] >= '0' && _s[_pos] <= '9') || _s[_pos] == '.' ||
                                    _s[_pos] == 'e' || _s[_pos] == 'E' || _s[_pos] == '+' || _s[_pos] == '-'))
            _pos++;
        std::string num = _s.substr(start, _pos - start);
        char* end = NULL;
        double v = strtod(num.c_str(), &end);
        if (end == num.c_str() || *end != '\0') { _pos = start; Fail("invalid number"); return false; }
        out._type = JsonValue::Number;
        out._number = v;
        return true;
    }

    bool ParseHex4(unsigned int& cp)
    {
        if (_pos + 4 > _s.size()) return false;
        cp = 0;
        for (int i = 0; i < 4; i++)
        {
            char c = _s[_pos + i];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= (unsigned int)(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= (unsigned int)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= (unsigned int)(c - 'A' + 10);
            else return false;
        }
        _pos += 4;
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned int cp)
    {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    bool ParseString(std::string& out)
    {
        _pos++; // opening quote
        out.clear();
        while (_pos < _s.size())
        {
            char c = _s[_pos];
            if (c == '"') { _pos++; return true; }
            if ((unsigned char)c < 0x20) { Fail("control character in string"); return false; }
            if (c != '\\') { out += c; _pos++; continue; }

            _pos++;
            if (_pos >= _s.size()) break;
            char e = _s[_pos++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                unsigned int cp;
                if (!ParseHex4(cp)) { Fail("invalid \\u escape"); return false; }
                if (cp >= 0xD800 && cp <= 0xDBFF && _pos + 1 < _s.size() && _s[_pos] == '\\' && _s[_pos + 1] == 'u')
                {
                    size_t save = _pos;
                    _pos += 2;
                    unsigned int lo;
                    if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        _pos = save;
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                _pos--;
                Fail("invalid escape");
                return false;
            }
        }
        Fail("unterminated string");
        return false;
    }

    bool ParseArray(JsonValue& out, int depth)
    {
        out._type = JsonValue::Array;
        _pos++; // [
        SkipWs();
        if (_pos < _s.size() && _s[_pos] == ']') { _pos++; return true; }
        while (true)
        {
            SkipWs();
            out._items.push_back(JsonValue());
            if (!ParseValue(out._items.back(), depth + 1)) return false;
            SkipWs();
            if (_pos >= _s.size()) { Fail("unterminated array"); return false; }
            if (_s[_pos] == ',') { _pos++; continue; }
            if (_s[_pos] == ']') { _pos++; return true; }
            Fail("expected ',' or ']'");
            return false;
        }
    }

    bool ParseObject(JsonValue& out, int depth)
    {
        out._type = JsonValue::Object;
        _pos++; // {
        SkipWs();
        if (_pos < _s.size() && _s[_pos] == '}') { _pos++; return true; }
        while (true)
        {
            SkipWs();
            if (_pos >= _s.size() || _s[_pos] != '"') { Fail("expected string key"); return false; }
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (_pos >= _s.size() || _s[_pos] != ':') { Fail("expected ':'"); return false; }
            _pos++;
            SkipWs();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            // Last duplicate key wins
            bool replaced = false;
            for (size_t i = 0; i < out._keys.size(); i++)
            {
                if (out._keys[i] == key) { out._items[i] = value; replaced = true; break; }
            }
            if (!replaced) { out._keys.push_back(key); out._items.push_back(value); }
            SkipWs();
            if (_pos >= _s.size()) { Fail("unterminated object"); return false; }
            if (_s[_pos] == ',') { _pos++; continue; }
            if (_s[_pos] == '}') { _pos++; return true; }
            Fail("expected ',' or '}'");
            return false;
        }
    }
};

bool JsonValue::Parse(const std::string& text, JsonValue& out, std::string& error)
{
    out = JsonValue();
    JsonParser parser(text);
    return parser.ParseDocument(out, error);
}

const JsonValue* JsonValue::Find(const std::string& key) const
{
    if (_type != Object) return NULL;
    for (size_t i = 0; i < _keys.size(); i++)
    {
        if (_keys[i] == key) return &_items[i];
    }
    return NULL;
}

std::string JsonQuote(const std::string& s)
{
    std::string out = "\"";
    out.reserve(s.size() + 2);
    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += (char)c;
            }
        }
    }
    out += "\"";
    return out;
}

std::string JsonNumber(double v)
{
    if (!std::isfinite(v)) return "null";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string JsonStringArray(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (size_t i = 0; i < items.size(); i++)
    {
        if (i > 0) out += ",";
        out += JsonQuote(items[i]);
    }
    return out + "]";
}
