#include "coatheal/telemetry_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <utility>

#ifdef COATHEAL_HAS_ZLIB
#include <zlib.h>
#endif

namespace coatheal {

namespace generated {
extern const unsigned char kTelemetryDictionaryZ1[];
extern const std::size_t kTelemetryDictionaryZ1Size;
}  // namespace generated

namespace {

constexpr char kZ1Prefix[] = "Z1,";
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

bool TelemetryCodecAvailable() {
#ifdef COATHEAL_HAS_ZLIB
  return true;
#else
  return false;
#endif
}

const std::string& TelemetryDictionaryZ1() {
  static const std::string dictionary(
      reinterpret_cast<const char*>(generated::kTelemetryDictionaryZ1),
      generated::kTelemetryDictionaryZ1Size);
  return dictionary;
}

std::string TelemetryDictionaryZ1CrcHex() {
#ifdef COATHEAL_HAS_ZLIB
  const std::string& dictionary = TelemetryDictionaryZ1();
  const uLong crc = crc32(crc32(0L, Z_NULL, 0),
                          reinterpret_cast<const Bytef*>(dictionary.data()),
                          static_cast<uInt>(dictionary.size()));
  std::array<char, 9> hex{};
  std::snprintf(hex.data(), hex.size(), "%08lx", static_cast<unsigned long>(crc));
  return std::string(hex.data());
#else
  return {};
#endif
}

std::string EncodeTelemetryLineZ1(const std::string& line) {
#ifdef COATHEAL_HAS_ZLIB
  const std::string& dictionary = TelemetryDictionaryZ1();
  z_stream stream{};
  // Raw DEFLATE (negative window bits: no zlib header or checksum), level 9,
  // memory level 9 -- the parameters docs/link-budget.md fixes for both ends.
  if (deflateInit2(&stream, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }
  std::string compressed;
  bool ok = deflateSetDictionary(&stream,
                                 reinterpret_cast<const Bytef*>(dictionary.data()),
                                 static_cast<uInt>(dictionary.size())) == Z_OK;
  if (ok) {
    compressed.resize(deflateBound(&stream, static_cast<uLong>(line.size())) + 16);
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(line.data()));
    stream.avail_in = static_cast<uInt>(line.size());
    stream.next_out = reinterpret_cast<Bytef*>(&compressed[0]);
    stream.avail_out = static_cast<uInt>(compressed.size());
    ok = deflate(&stream, Z_FINISH) == Z_STREAM_END;
    compressed.resize(stream.total_out);
  }
  deflateEnd(&stream);
  if (!ok) return {};
  return std::string(kZ1Prefix) + Base64Encode(compressed);
#else
  (void)line;
  return {};
#endif
}

bool DecodeTelemetryLineZ1(const std::string& wire_line, std::string* line) {
#ifdef COATHEAL_HAS_ZLIB
  if (line == nullptr || wire_line.rfind(kZ1Prefix, 0) != 0) return false;
  std::string compressed;
  if (!Base64Decode(wire_line.substr(sizeof(kZ1Prefix) - 1), &compressed) ||
      compressed.empty()) {
    return false;
  }
  const std::string& dictionary = TelemetryDictionaryZ1();
  z_stream stream{};
  if (inflateInit2(&stream, -15) != Z_OK) return false;
  bool ok = inflateSetDictionary(&stream,
                                 reinterpret_cast<const Bytef*>(dictionary.data()),
                                 static_cast<uInt>(dictionary.size())) == Z_OK;
  std::string out;
  std::array<char, 4096> chunk{};
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  while (ok) {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    const int rc = inflate(&stream, Z_NO_FLUSH);
    out.append(chunk.data(), chunk.size() - stream.avail_out);
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK) ok = false;
  }
  inflateEnd(&stream);
  if (!ok) return false;
  *line = std::move(out);
  return true;
#else
  (void)wire_line;
  (void)line;
  return false;
#endif
}

std::string Base64Encode(const std::string& bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                       (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                       static_cast<unsigned char>(bytes[i + 2]);
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(kAlphabet[(v >> 6) & 0x3F]);
    out.push_back(kAlphabet[v & 0x3F]);
  }
  const std::size_t rest = bytes.size() - i;
  if (rest == 1) {
    const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.append("==");
  } else if (rest == 2) {
    const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                       (static_cast<unsigned char>(bytes[i + 1]) << 8);
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(kAlphabet[(v >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

bool Base64Decode(const std::string& text, std::string* bytes) {
  if (bytes == nullptr || text.size() % 4 != 0) return false;
  std::string out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    std::size_t padding = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      const char c = text[i + k];
      if (c == '=' && i + 4 == text.size() && k >= 2) {
        v[k] = 0;
        ++padding;
      } else if (padding > 0 || (v[k] = Base64Value(c)) < 0) {
        return false;
      }
    }
    const unsigned triple = (static_cast<unsigned>(v[0]) << 18) |
                            (static_cast<unsigned>(v[1]) << 12) |
                            (static_cast<unsigned>(v[2]) << 6) |
                            static_cast<unsigned>(v[3]);
    out.push_back(static_cast<char>((triple >> 16) & 0xFF));
    if (padding < 2) out.push_back(static_cast<char>((triple >> 8) & 0xFF));
    if (padding < 1) out.push_back(static_cast<char>(triple & 0xFF));
  }
  *bytes = std::move(out);
  return true;
}

}  // namespace coatheal
