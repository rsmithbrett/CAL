#pragma once
// The part of ArduinoJson that App/Aircraft.cpp uses, over a small JSON reader,
// so the shipped fetchMine() and parseRefusal() run on a host against the
// bodies a test hands them. The host tests run before any Arduino library is
// installed, which is why this stands in for the library.
//
// Two differences from the library that a test using this must know:
//
//   - A filter is accepted and not applied. Every field in the body reaches the
//     document, so this cannot show a field missing from fetchMine()'s filter.
//   - Nothing here allocates from a pool or measures one. Heap behaviour is not
//     something this stub can speak to.
#include <Arduino.h>

#include <stdlib.h>

#include <string>
#include <utility>
#include <vector>

namespace FakeJson {

struct Value {
  enum class Kind { Null, Boolean, Number, Text, Array, Object };
  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> members;
};

class Reader {
 public:
  explicit Reader(const std::string& source) : source_(source) {}

  /// True when the whole of the source is one JSON value.
  bool read(Value& into) {
    skipSpace();
    if (!readValue(into)) {
      return false;
    }
    skipSpace();
    return at_ == source_.size();
  }

 private:
  void skipSpace() {
    while (at_ < source_.size() && (source_[at_] == ' ' || source_[at_] == '\n' ||
                                    source_[at_] == '\r' || source_[at_] == '\t')) {
      at_++;
    }
  }

  bool take(char expected) {
    if (at_ < source_.size() && source_[at_] == expected) {
      at_++;
      return true;
    }
    return false;
  }

  bool takeWord(const char* word) {
    const size_t length = strlen(word);
    if (source_.compare(at_, length, word) != 0) {
      return false;
    }
    at_ += length;
    return true;
  }

  bool readText(std::string& into) {
    if (!take('"')) {
      return false;
    }
    while (at_ < source_.size() && source_[at_] != '"') {
      char next = source_[at_++];
      if (next == '\\') {
        if (at_ >= source_.size()) {
          return false;
        }
        const char escaped = source_[at_++];
        next = escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
      }
      into.push_back(next);
    }
    return take('"');
  }

  bool readValue(Value& into) {
    if (at_ >= source_.size()) {
      return false;
    }
    const char first = source_[at_];
    if (first == '{') {
      return readObject(into);
    }
    if (first == '[') {
      return readArray(into);
    }
    if (first == '"') {
      into.kind = Value::Kind::Text;
      return readText(into.text);
    }
    if (takeWord("true")) {
      into.kind = Value::Kind::Boolean;
      into.boolean = true;
      return true;
    }
    if (takeWord("false")) {
      into.kind = Value::Kind::Boolean;
      return true;
    }
    if (takeWord("null")) {
      return true;
    }
    const char* const start = source_.c_str() + at_;
    char* end = nullptr;
    const double number = strtod(start, &end);
    if (end == start) {
      return false;
    }
    at_ += static_cast<size_t>(end - start);
    into.kind = Value::Kind::Number;
    into.number = number;
    return true;
  }

  bool readObject(Value& into) {
    into.kind = Value::Kind::Object;
    take('{');
    skipSpace();
    if (take('}')) {
      return true;
    }
    for (;;) {
      skipSpace();
      std::string key;
      if (!readText(key)) {
        return false;
      }
      skipSpace();
      if (!take(':')) {
        return false;
      }
      skipSpace();
      Value member;
      if (!readValue(member)) {
        return false;
      }
      into.members.emplace_back(std::move(key), std::move(member));
      skipSpace();
      if (take('}')) {
        return true;
      }
      if (!take(',')) {
        return false;
      }
    }
  }

  bool readArray(Value& into) {
    into.kind = Value::Kind::Array;
    take('[');
    skipSpace();
    if (take(']')) {
      return true;
    }
    for (;;) {
      skipSpace();
      Value item;
      if (!readValue(item)) {
        return false;
      }
      into.items.push_back(std::move(item));
      skipSpace();
      if (take(']')) {
        return true;
      }
      if (!take(',')) {
        return false;
      }
    }
  }

  const std::string& source_;
  size_t at_ = 0;
};

}  // namespace FakeJson

class JsonArrayConst;

/// One value in a document, or nothing. Reading through nothing answers
/// nothing, which is how a missing field falls through to the `|` default.
class JsonVariantConst {
 public:
  JsonVariantConst() = default;
  explicit JsonVariantConst(const FakeJson::Value* value) : value_(value) {}

  JsonVariantConst operator[](const char* key) const {
    if (value_ == nullptr || value_->kind != FakeJson::Value::Kind::Object) {
      return JsonVariantConst();
    }
    for (const auto& member : value_->members) {
      if (member.first == key) {
        return JsonVariantConst(&member.second);
      }
    }
    return JsonVariantConst();
  }

  JsonVariantConst operator[](int index) const {
    if (value_ == nullptr || value_->kind != FakeJson::Value::Kind::Array || index < 0 ||
        static_cast<size_t>(index) >= value_->items.size()) {
      return JsonVariantConst();
    }
    return JsonVariantConst(&value_->items[static_cast<size_t>(index)]);
  }

  bool isNull() const { return value_ == nullptr || value_->kind == FakeJson::Value::Kind::Null; }

  template <typename T>
  T as() const;

  template <typename T>
  bool is() const;

  /// What fetchMine() writes while it builds its filter. Accepted and dropped,
  /// as the note at the top of this file says.
  const JsonVariantConst& operator=(bool) const { return *this; }

  const FakeJson::Value* value() const { return value_; }

 private:
  const FakeJson::Value* value_ = nullptr;
};

class JsonArrayConst {
 public:
  JsonArrayConst() = default;
  explicit JsonArrayConst(const FakeJson::Value* value) : value_(value) {}

  bool isNull() const { return value_ == nullptr; }
  size_t size() const { return value_ == nullptr ? 0 : value_->items.size(); }
  JsonVariantConst operator[](int index) const { return JsonVariantConst(value_)[index]; }

  class Iterator {
   public:
    Iterator(const FakeJson::Value* value, size_t at) : value_(value), at_(at) {}
    JsonVariantConst operator*() const { return JsonVariantConst(&value_->items[at_]); }
    Iterator& operator++() {
      at_++;
      return *this;
    }
    bool operator!=(const Iterator& other) const { return at_ != other.at_; }

   private:
    const FakeJson::Value* value_;
    size_t at_;
  };

  Iterator begin() const { return Iterator(value_, 0); }
  Iterator end() const { return Iterator(value_, size()); }

 private:
  const FakeJson::Value* value_ = nullptr;
};

template <>
inline bool JsonVariantConst::as<bool>() const {
  return value_ != nullptr && value_->kind == FakeJson::Value::Kind::Boolean && value_->boolean;
}

template <>
inline double JsonVariantConst::as<double>() const {
  return value_ != nullptr && value_->kind == FakeJson::Value::Kind::Number ? value_->number : 0.0;
}

template <>
inline JsonArrayConst JsonVariantConst::as<JsonArrayConst>() const {
  return value_ != nullptr && value_->kind == FakeJson::Value::Kind::Array
             ? JsonArrayConst(value_)
             : JsonArrayConst();
}

template <>
inline bool JsonVariantConst::is<double>() const {
  return value_ != nullptr && value_->kind == FakeJson::Value::Kind::Number;
}

inline const char* operator|(const JsonVariantConst& variant, const char* fallback) {
  const FakeJson::Value* value = variant.value();
  return value != nullptr && value->kind == FakeJson::Value::Kind::Text ? value->text.c_str()
                                                                        : fallback;
}

inline int operator|(const JsonVariantConst& variant, int fallback) {
  const FakeJson::Value* value = variant.value();
  return value != nullptr && value->kind == FakeJson::Value::Kind::Number
             ? static_cast<int>(value->number)
             : fallback;
}

inline double operator|(const JsonVariantConst& variant, double fallback) {
  const FakeJson::Value* value = variant.value();
  return value != nullptr && value->kind == FakeJson::Value::Kind::Number ? value->number
                                                                          : fallback;
}

class JsonDocument {
 public:
  JsonVariantConst operator[](const char* key) const { return JsonVariantConst(&root_)[key]; }
  FakeJson::Value& root() { return root_; }

 private:
  FakeJson::Value root_;
};

class DeserializationError {
 public:
  explicit DeserializationError(bool failed) : failed_(failed) {}
  explicit operator bool() const { return failed_; }

 private:
  bool failed_;
};

namespace DeserializationOption {
class Filter {
 public:
  explicit Filter(const JsonDocument&) {}
};
}  // namespace DeserializationOption

inline DeserializationError deserializeJson(JsonDocument& document, const std::string& body) {
  document.root() = FakeJson::Value();
  return DeserializationError(!FakeJson::Reader(body).read(document.root()));
}

inline DeserializationError deserializeJson(JsonDocument& document, Stream& stream,
                                            DeserializationOption::Filter) {
  return deserializeJson(document, stream.body);
}
