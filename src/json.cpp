#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace json {

const Value& Value::operator[](const std::string& key) const {
  static const Value null;
  if (!isObj()) return null;
  auto it = o->find(key);
  return it == o->end() ? null : it->second;
}

Value Value::clone() const {
  if (isArr()) {
    Array out;
    for (auto& v : *a) out.push_back(v.clone());
    return Value(std::move(out));
  }
  if (isObj()) {
    Object out;
    for (auto& [k, v] : *o) out[k] = v.clone();
    return Value(std::move(out));
  }
  return *this;
}

namespace {

struct Parser {
  const std::string& t;
  size_t i = 0;

  [[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("json: ") + what + " at offset " + std::to_string(i));
  }
  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
  }
  bool lit(const char* s) {
    size_t n = strlen(s);
    if (t.compare(i, n, s) == 0) { i += n; return true; }
    return false;
  }

  static void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
    }
  }

  unsigned hex4() {
    if (i + 4 > t.size()) fail("bad \\u escape");
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
      char c = t[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else fail("bad hex digit");
    }
    return v;
  }

  std::string string() {
    if (t[i] != '"') fail("expected string");
    i++;
    std::string out;
    while (true) {
      if (i >= t.size()) fail("unterminated string");
      char c = t[i++];
      if (c == '"') break;
      if (c != '\\') { out += c; continue; }
      if (i >= t.size()) fail("bad escape");
      char e = t[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = hex4();
          if (cp >= 0xD800 && cp < 0xDC00 && lit("\\u")) {
            unsigned lo = hex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default: fail("bad escape");
      }
    }
    return out;
  }

  Value value() {
    ws();
    if (i >= t.size()) fail("unexpected end");
    char c = t[i];
    if (c == '{') {
      i++;
      Object obj;
      ws();
      if (i < t.size() && t[i] == '}') { i++; return Value(std::move(obj)); }
      while (true) {
        ws();
        std::string key = string();
        ws();
        if (i >= t.size() || t[i] != ':') fail("expected ':'");
        i++;
        obj[key] = value();
        ws();
        if (i < t.size() && t[i] == ',') { i++; continue; }
        if (i < t.size() && t[i] == '}') { i++; break; }
        fail("expected ',' or '}'");
      }
      return Value(std::move(obj));
    }
    if (c == '[') {
      i++;
      Array arr;
      ws();
      if (i < t.size() && t[i] == ']') { i++; return Value(std::move(arr)); }
      while (true) {
        arr.push_back(value());
        ws();
        if (i < t.size() && t[i] == ',') { i++; continue; }
        if (i < t.size() && t[i] == ']') { i++; break; }
        fail("expected ',' or ']'");
      }
      return Value(std::move(arr));
    }
    if (c == '"') return Value(string());
    if (lit("true")) return Value(true);
    if (lit("false")) return Value(false);
    if (lit("null")) return Value();
    size_t start = i;
    while (i < t.size() && (isdigit((unsigned char)t[i]) || strchr("+-.eE", t[i]))) i++;
    if (start == i) fail("unexpected character");
    return Value(std::stod(t.substr(start, i - start)));
  }
};

void dumpTo(std::string& out, const Value& v, bool pretty, int indent) {
  auto nl = [&](int ind) {
    if (!pretty) return;
    out += '\n';
    out.append(ind * 2, ' ');
  };
  switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.b ? "true" : "false"; break;
    case Value::Type::Number: {
      if (!std::isfinite(v.n)) { out += "null"; break; }
      char buf[32];
      if (v.n == 0) snprintf(buf, sizeof buf, "0");
      else if (v.n == std::floor(v.n) && std::fabs(v.n) < 1e15) snprintf(buf, sizeof buf, "%.0f", v.n);
      else snprintf(buf, sizeof buf, "%.12g", v.n);  // enough for QPC-based ms timestamps
      out += buf;
      break;
    }
    case Value::Type::String: out += '"'; out += escape(v.s); out += '"'; break;
    case Value::Type::Array: {
      out += '[';
      // Arrays of scalars stay on one line even when pretty.
      bool flat = true;
      for (auto& e : *v.a) if (e.isArr() || e.isObj()) flat = false;
      bool first = true;
      for (auto& e : *v.a) {
        if (!first) out += pretty && flat ? ", " : ",";
        if (pretty && !flat) nl(indent + 1);
        dumpTo(out, e, pretty && !flat, indent + 1);
        first = false;
      }
      if (pretty && !flat && !v.a->empty()) nl(indent);
      out += ']';
      break;
    }
    case Value::Type::Object: {
      out += '{';
      bool first = true;
      for (auto& [k, e] : *v.o) {
        if (!first) out += ',';
        nl(indent + 1);
        out += '"'; out += escape(k); out += pretty ? "\": " : "\":";
        dumpTo(out, e, pretty, indent + 1);
        first = false;
      }
      if (!v.o->empty()) nl(indent);
      out += '}';
      break;
    }
  }
}

}  // namespace

Value parse(const std::string& text) {
  Parser p{text};
  Value v = p.value();
  p.ws();
  if (p.i != text.size()) p.fail("trailing characters");
  return v;
}

std::string dump(const Value& v, bool pretty) {
  std::string out;
  dumpTo(out, v, pretty, 0);
  return out;
}

std::string escape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  return out;
}

}  // namespace json
