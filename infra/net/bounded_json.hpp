#pragma once
#include "http.hpp"
#include <map>
#include <variant>
#include <vector>

namespace gsdl::net::json {
struct Number {
  std::string text;
};
struct Value {
  using Object = std::map<std::string, Value, std::less<>>;
  using Array = std::vector<Value>;
  std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> data;
  const Value &at(std::string_view key) const {
    auto o = std::get_if<Object>(&data);
    if (!o)
      throw Error(L"来源清单对象结构改变");
    auto i = o->find(key);
    if (i == o->end())
      throw Error(L"来源清单缺少必要字段");
    return i->second;
  }
  const std::string &text() const {
    auto p = std::get_if<std::string>(&data);
    if (!p)
      throw Error(L"来源清单字段类型改变");
    return *p;
  }
  const Array &array() const {
    auto p = std::get_if<Array>(&data);
    if (!p)
      throw Error(L"来源资源列表类型改变");
    return *p;
  }
  bool number(std::string_view n) const {
    auto p = std::get_if<Number>(&data);
    return p && p->text == n;
  }
};
// Strict JSON grammar, duplicate-key rejection and explicit allocation limits.
// Numbers retain their original decimal token; there is no float conversion.
class Parser {
  std::string_view s;
  std::size_t p{}, nodes{};
  [[noreturn]] void bad() const {
    throw Error(L"来源 JSON 无效、字段重复或超过读取限制");
  }
  void ws() {
    while (p < s.size() &&
           (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t'))
      ++p;
  }
  bool take(char c) {
    ws();
    if (p < s.size() && s[p] == c) {
      ++p;
      return true;
    }
    return false;
  }
  unsigned hex() {
    unsigned v{};
    for (int i = 0; i < 4; ++i) {
      if (p == s.size())
        bad();
      char c = s[p++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v += c - '0';
      else if (c >= 'a' && c <= 'f')
        v += c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        v += c - 'A' + 10;
      else
        bad();
    }
    return v;
  }
  std::string string() {
    if (!take('"'))
      bad();
    std::string out;
    while (p < s.size()) {
      unsigned char c = s[p++];
      if (c == '"')
        return out;
      if (c < 32)
        bad();
      if (c != '\\')
        out += static_cast<char>(c);
      else {
        if (p == s.size())
          bad();
        switch (s[p++]) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          auto x = hex();
          std::wstring w(1, static_cast<wchar_t>(x));
          if (x >= 0xd800 && x <= 0xdbff) {
            if (p + 2 > s.size() || s.substr(p, 2) != "\\u")
              bad();
            p += 2;
            auto y = hex();
            if (y < 0xdc00 || y > 0xdfff)
              bad();
            w += static_cast<wchar_t>(y);
          } else if (x >= 0xdc00 && x <= 0xdfff)
            bad();
          if (x == 0)
            bad();
          out += utf8(w);
          break;
        }
        default:
          bad();
        }
      }
      if (out.size() > 1024 * 1024)
        bad();
    }
    bad();
  }
  bool digit() const { return p < s.size() && s[p] >= '0' && s[p] <= '9'; }
  Value value(unsigned depth) {
    ws();
    if (p == s.size() || depth > 64 || ++nodes > 200000)
      bad();
    if (s[p] == '"')
      return {string()};
    if (take('{')) {
      Value::Object o;
      if (take('}'))
        return {o};
      do {
        auto k = string();
        if (k.size() > 1024 || !take(':'))
          bad();
        auto v = value(depth + 1);
        if (!o.emplace(std::move(k), std::move(v)).second || o.size() > 10000)
          bad();
        if (take('}'))
          return {std::move(o)};
      } while (take(','));
      bad();
    }
    if (take('[')) {
      Value::Array a;
      if (take(']'))
        return {a};
      do {
        a.push_back(value(depth + 1));
        if (a.size() > 10000)
          bad();
        if (take(']'))
          return {std::move(a)};
      } while (take(','));
      bad();
    }
    for (auto [token, result] :
         {std::pair<std::string_view, Value>{"null", {nullptr}},
          {"true", {true}},
          {"false", {false}}})
      if (s.substr(p, token.size()) == token) {
        p += token.size();
        return result;
      }
    auto start = p;
    if (s[p] == '-')
      ++p;
    if (!digit())
      bad();
    if (s[p] == '0')
      ++p;
    else
      while (digit())
        ++p;
    if (p < s.size() && s[p] == '.') {
      ++p;
      if (!digit())
        bad();
      while (digit())
        ++p;
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      ++p;
      if (p < s.size() && (s[p] == '+' || s[p] == '-'))
        ++p;
      if (!digit())
        bad();
      while (digit())
        ++p;
    }
    if (p - start > 128)
      bad();
    return {Number{std::string(s.substr(start, p - start))}};
  }

public:
  explicit Parser(std::string_view input) : s(input) {
    if (s.size() > 8 * 1024 * 1024)
      bad();
    wide(input);
  }
  Value parse() {
    auto v = value(0);
    ws();
    if (p != s.size())
      bad();
    return v;
  }
};
} // namespace gsdl::net::json
