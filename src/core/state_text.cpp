// The two number routines every built-in plugin's state blob goes through.
// In a file of their own rather than in plugin.cpp so a plugin extracted from
// the host - the Drone as a CLAP - can carry them without carrying the
// backends, the registry and JACK along.
#include "core/plugin.h"

#include <algorithm>
#include <charconv>

namespace nirbija {

bool parse_number(std::string_view text, double* out) {
  // Leading blanks are the one thing from_chars will not skip, and a state
  // blob written with a space after its separator is not corrupt.
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                           text.front() == '\r' || text.front() == '\n'))
    text.remove_prefix(1);
  if (text.empty()) return false;

  // A number written under a locale whose decimal mark is a comma. The
  // writers here no longer produce one, but sessions written before they
  // stopped are on disk, and "0,5" read as 0 is how every gate in them
  // collapsed to the floor.
  char fixed[64];
  if (text.size() < sizeof(fixed) &&
      text.find(',') != std::string_view::npos &&
      text.find('.') == std::string_view::npos) {
    for (size_t i = 0; i < text.size(); ++i)
      fixed[i] = text[i] == ',' ? '.' : text[i];
    text = std::string_view(fixed, text.size());
  }

  double value = 0.0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{}) return false;
  if (out != nullptr) *out = value;
  return true;
}

std::string format_number(double value, int decimals) {
  char buffer[64];
  const auto result = std::to_chars(
      buffer, buffer + sizeof(buffer), value, std::chars_format::fixed,
      std::clamp(decimals, 0, 20));
  if (result.ec != std::errc{}) return "0";
  return std::string(buffer, result.ptr);
}

}  // namespace nirbija
