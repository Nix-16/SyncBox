#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace syncbox::audit {

// Audit sink. configure("") logs JSON lines to stderr; a path logs to that
// file. Only hashed token prefixes are ever written (SECURITY.md §8).
void configure(std::string_view log_file);
void reset();
void flush();

void event(std::string_view name,
           std::vector<std::pair<std::string, std::string>> fields = {});

// Hashes the token and logs only the first 8 hex characters.
void auth_failure_for_token(std::string_view token, std::string_view reason);

void device_revoked(std::string_view device_id, std::string_view actor,
                    std::string_view reason);

}  // namespace syncbox::audit
