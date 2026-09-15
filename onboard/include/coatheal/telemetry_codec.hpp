#pragma once

#include <cstdint>
#include <string>

namespace coatheal {

// Z1 telemetry line compression (docs/link-budget.md "Telemetry framing"):
// `Z1,<base64>` of the raw DEFLATE stream of one text line, compressed
// against the preset dictionary protocol/telemetry-dictionary-z1.txt (built
// into the binary). A typical DATA line goes from ~1.15 kB to ~0.24 kB.

// False when the binary was built without zlib: the link then stays plain.
bool TelemetryCodecAvailable();

const std::string& TelemetryDictionaryZ1();
// CRC-32 of the dictionary as 8 lowercase hex digits; the telemetry HELLO
// carries it so both ends provably compress against the same bytes.
std::string TelemetryDictionaryZ1CrcHex();

// `Z1,<base64>` for `line` (no trailing newline), or "" if compression fails.
std::string EncodeTelemetryLineZ1(const std::string& line);
// Inverse of EncodeTelemetryLineZ1, for tests and diagnostics.
bool DecodeTelemetryLineZ1(const std::string& wire_line, std::string* line);

std::string Base64Encode(const std::string& bytes);
bool Base64Decode(const std::string& text, std::string* bytes);

}  // namespace coatheal
