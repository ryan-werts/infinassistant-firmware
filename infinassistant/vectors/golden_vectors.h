// Reads protocol-vectors.json, the golden vectors the firmware host tests, the app's tests and the
// Home Assistant tests share. Host tests only: this directory is outside src/, so it is never
// copied into the firmware tree.
//
// A vector is looked up by name. One that is missing or malformed stops the test with a message
// and exit code 1, so a vector renamed or removed from the file fails the tests that pin it.
//
// The parser reads the file as written by the generator: every vector has `"name": "..."`, and
// its `"hex"` (space-separated bytes) and `"value"` (a decimal number) follow before the next
// `"name"`. It is not a general JSON parser.
//
// GOLDEN_VECTORS_JSON (the file's path) is defined by tools/fuzz/CMakeLists.txt.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef GOLDEN_VECTORS_JSON
#error "GOLDEN_VECTORS_JSON must name protocol-vectors.json"
#endif

namespace golden {

[[noreturn]] inline void Fail(const std::string& why) {
  std::printf("  FAIL golden vectors: %s\n", why.c_str());
  std::exit(1);
}

inline const std::string& File() {
  static const std::string text = [] {
    std::FILE* f = std::fopen(GOLDEN_VECTORS_JSON, "rb");
    if (f == nullptr) Fail(std::string("cannot open ") + GOLDEN_VECTORS_JSON);
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
  }();
  return text;
}

/** The text of the vector `name`: from its "name" key to the next vector's. */
inline std::string Section(const std::string& name) {
  const std::string& text = File();
  const std::string key = "\"name\": \"" + name + "\"";
  const size_t at = text.find(key);
  if (at == std::string::npos) Fail("no vector named " + name);
  const size_t next = text.find("\"name\": \"", at + key.size());
  return text.substr(at, next == std::string::npos ? std::string::npos : next - at);
}

/** The vector's "hex" bytes. */
inline std::vector<uint8_t> Bytes(const std::string& name) {
  const std::string sec = Section(name);
  const std::string key = "\"hex\": \"";
  const size_t at = sec.find(key);
  if (at == std::string::npos) Fail(name + " has no hex");
  const size_t end = sec.find('"', at + key.size());
  if (end == std::string::npos) Fail(name + " has an unterminated hex");
  std::vector<uint8_t> out;
  int hi = -1;
  for (size_t i = at + key.size(); i < end; i++) {
    const char c = sec[i];
    if (c == ' ') continue;
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else { Fail(name + " has a bad hex digit"); }
    if (hi < 0) {
      hi = d;
    } else {
      out.push_back(static_cast<uint8_t>(hi * 16 + d));
      hi = -1;
    }
  }
  if (hi >= 0) Fail(name + " has an odd number of hex digits");
  return out;
}

/** The vector's "value". */
inline uint64_t Value(const std::string& name) {
  const std::string sec = Section(name);
  const std::string key = "\"value\": ";
  const size_t at = sec.find(key);
  if (at == std::string::npos) Fail(name + " has no value");
  return std::strtoull(sec.c_str() + at + key.size(), nullptr, 10);
}

}  // namespace golden
