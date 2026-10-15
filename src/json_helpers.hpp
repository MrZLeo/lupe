#ifndef LUPE_JSON_HELPERS_HPP_
#define LUPE_JSON_HELPERS_HPP_

#include <simdjson.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lupe::detail {

inline bool element_at(simdjson::dom::element value, std::string_view pointer,
                       simdjson::dom::element &field) {
  return !value.at_pointer(pointer).get(field);
}

inline std::optional<std::string>
string_at(simdjson::dom::element value, std::string_view pointer) {
  simdjson::dom::element field;
  if (!element_at(value, pointer, field)) {
    return std::nullopt;
  }

  std::string_view text;
  if (field.get_string().get(text)) {
    return std::nullopt;
  }
  return std::string{text};
}

inline std::optional<std::uint64_t>
uint_at(simdjson::dom::element value, std::string_view pointer) {
  simdjson::dom::element field;
  if (!element_at(value, pointer, field)) {
    return std::nullopt;
  }

  std::uint64_t number = 0;
  if (!field.get_uint64().get(number)) {
    return number;
  }

  std::int64_t signed_number = 0;
  if (!field.get_int64().get(signed_number) && signed_number >= 0) {
    return static_cast<std::uint64_t>(signed_number);
  }
  return std::nullopt;
}

inline std::optional<double>
double_at(simdjson::dom::element value, std::string_view pointer) {
  simdjson::dom::element field;
  if (!element_at(value, pointer, field)) {
    return std::nullopt;
  }

  double number = 0.0;
  if (!field.get_double().get(number)) {
    return number;
  }
  return std::nullopt;
}

inline std::optional<bool>
bool_at(simdjson::dom::element value, std::string_view pointer) {
  simdjson::dom::element field;
  if (!element_at(value, pointer, field)) {
    return std::nullopt;
  }

  bool boolean = false;
  if (!field.get_bool().get(boolean)) {
    return boolean;
  }
  return std::nullopt;
}

inline std::string json_text(simdjson::dom::element value) {
  return simdjson::to_string(value);
}

inline std::string
json_at(simdjson::dom::element value, std::string_view pointer) {
  simdjson::dom::element field;
  if (!element_at(value, pointer, field)) {
    return {};
  }
  return json_text(field);
}

inline std::string
scalar_text_at(simdjson::dom::element value, std::string_view pointer) {
  simdjson::dom::element field;
  if (!element_at(value, pointer, field)) {
    return {};
  }

  std::string_view text;
  if (!field.get_string().get(text)) {
    return std::string{text};
  }

  switch (field.type()) {
  case simdjson::dom::element_type::INT64:
  case simdjson::dom::element_type::UINT64:
  case simdjson::dom::element_type::DOUBLE:
  case simdjson::dom::element_type::BOOL:
  case simdjson::dom::element_type::BIGINT:
    return json_text(field);
  default:
    return {};
  }
}

} // namespace lupe::detail

#endif // LUPE_JSON_HELPERS_HPP_
