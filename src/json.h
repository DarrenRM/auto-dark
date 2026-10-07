// Minimal JSON value + parser + serializer. Enough for params, profiles and the HTTP API.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::shared_ptr<Array> a;
  std::shared_ptr<Object> o;

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool v) : type(Type::Bool), b(v) {}
  Value(double v) : type(Type::Number), n(v) {}
  Value(int v) : type(Type::Number), n(v) {}
  Value(const char* v) : type(Type::String), s(v) {}
  Value(std::string v) : type(Type::String), s(std::move(v)) {}
  Value(Array v) : type(Type::Array), a(std::make_shared<Array>(std::move(v))) {}
  Value(Object v) : type(Type::Object), o(std::make_shared<Object>(std::move(v))) {}

  bool isNull() const { return type == Type::Null; }
  bool isNum() const { return type == Type::Number; }
  bool isStr() const { return type == Type::String; }
  bool isArr() const { return type == Type::Array; }
  bool isObj() const { return type == Type::Object; }

  double num(double def = 0) const { return isNum() ? n : (type == Type::Bool ? (b ? 1 : 0) : def); }
  const std::string& str() const { return s; }
  const Array& arr() const { static const Array empty; return isArr() ? *a : empty; }
  const Object& obj() const { static const Object empty; return isObj() ? *o : empty; }

  // Object member lookup; returns a null value when missing.
  const Value& operator[](const std::string& key) const;
  bool has(const std::string& key) const { return isObj() && o->count(key) > 0; }

  // Deep copy (Arrays/Objects are shared_ptr-backed).
  Value clone() const;
};

// Throws std::runtime_error on malformed input.
Value parse(const std::string& text);
std::string dump(const Value& v, bool pretty = false);
std::string escape(const std::string& s);

}  // namespace json
