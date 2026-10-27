#include <syncbox/server/api.hpp>

#include <syncbox/common/time.hpp>
#include <syncbox/core/hash.hpp>
#include <syncbox/core/token.hpp>
#include <syncbox/core/uuid.hpp>

#include <boost/beast/http/verb.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncbox::server {

namespace {

namespace http = boost::beast::http;

struct ParsedTarget {
  std::string path;
  std::vector<std::string> segments;
  std::map<std::string, std::string> query;
};

[[nodiscard]] ParsedTarget parse_target(std::string_view target) {
  ParsedTarget parsed;
  const auto question = target.find('?');
  const std::string_view raw_path = target.substr(0, question);
  const std::string_view raw_query =
      question == std::string_view::npos ? std::string_view{} : target.substr(question + 1);
  parsed.path = std::string(raw_path);

  std::size_t start = 0;
  while (start < raw_path.size()) {
    if (raw_path[start] == '/') {
      ++start;
      continue;
    }
    const auto slash = raw_path.find('/', start);
    const auto end = slash == std::string_view::npos ? raw_path.size() : slash;
    parsed.segments.emplace_back(raw_path.substr(start, end - start));
    start = end;
  }

  start = 0;
  while (start < raw_query.size()) {
    const auto ampersand = raw_query.find('&', start);
    const auto end = ampersand == std::string_view::npos ? raw_query.size() : ampersand;
    const std::string_view pair = raw_query.substr(start, end - start);
    const auto equals = pair.find('=');
    if (equals != std::string_view::npos) {
      parsed.query.emplace(std::string(pair.substr(0, equals)),
                           std::string(pair.substr(equals + 1)));
    }
    start = end + 1;
  }
  return parsed;
}

[[nodiscard]] std::optional<std::string> header_value(const net::HttpRequest& request,
                                                      std::string_view name) {
  for (const auto& header : request.headers) {
    if (header.first.size() != name.size()) {
      continue;
    }
    bool equal = true;
    for (std::size_t i = 0; i < name.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(header.first[i])) !=
          std::tolower(static_cast<unsigned char>(name[i]))) {
        equal = false;
        break;
      }
    }
    if (equal) {
      return header.second;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> bearer_token(const net::HttpRequest& request) {
  const auto authorization = header_value(request, "authorization");
  if (!authorization.has_value()) {
    return std::nullopt;
  }
  constexpr std::string_view kPrefix = "Bearer ";
  if (authorization->size() <= kPrefix.size() ||
      authorization->compare(0, kPrefix.size(), kPrefix) != 0) {
    return std::nullopt;
  }
  return authorization->substr(kPrefix.size());
}

[[nodiscard]] nlohmann::json error_body(ErrorCode code, std::string_view message,
                                        const nlohmann::json& details) {
  nlohmann::json body;
  body["error"]["code"] = std::string(to_string(code));
  body["error"]["message"] = std::string(message);
  body["error"]["retryable"] = is_retryable(code);
  body["error"]["request_id"] = generate_uuid_v7();
  body["error"]["details"] = details;
  return body;
}

[[nodiscard]] net::HttpResponse error_response(ErrorCode code, std::string_view message,
                                               nlohmann::json details = nlohmann::json::object()) {
  int status = http_status(code);
  if (status == 0) {
    status = 500;
  }
  net::HttpResponse response;
  response.status = static_cast<unsigned>(status);
  response.body = error_body(code, message, details).dump();
  return response;
}

[[nodiscard]] net::HttpResponse error_response(const Error& error) {
  return error_response(error.code, error.message);
}

[[nodiscard]] net::HttpResponse json_response(unsigned status, const nlohmann::json& body) {
  net::HttpResponse response;
  response.status = status;
  response.body = body.dump();
  return response;
}

[[nodiscard]] nlohmann::json change_json(const ChangeRecord& change) {
  nlohmann::json value;
  value["seq"] = change.seq.value;
  value["file_id"] = change.file_id.value;
  value["revision"] = change.revision.value;
  value["op"] = std::string(to_string(change.op));
  value["path"] = change.path;
  value["content_hash"] =
      change.content_hash.has_value() ? nlohmann::json(change.content_hash->to_hex())
                                      : nlohmann::json(nullptr);
  value["size"] = change.size;
  value["deleted"] = change.deleted;
  value["conflict"] = change.conflict;
  value["device_id"] = change.device_id;
  value["created_at"] = change.created_at;
  return value;
}

[[nodiscard]] nlohmann::json entry_snapshot_json(const EntryRow& entry) {
  nlohmann::json value;
  value["file_id"] = entry.file_id.value;
  value["path"] = entry.path;
  value["revision"] = entry.revision.value;
  value["content_hash"] = entry.content_hash.has_value()
                              ? nlohmann::json(entry.content_hash->to_hex())
                              : nlohmann::json(nullptr);
  value["size"] = entry.size;
  value["deleted"] = entry.deleted;
  value["updated_seq"] = entry.updated_seq.value;
  return value;
}

[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view text) {
  try {
    std::size_t consumed = 0;
    const long long value = std::stoll(std::string(text), &consumed);
    if (consumed != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] bool is_get(const net::HttpRequest& request) {
  return request.method == http::verb::get;
}

[[nodiscard]] bool is_post(const net::HttpRequest& request) {
  return request.method == http::verb::post;
}

}  // namespace

net::HttpResponse Api::handle(const net::HttpRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  const ParsedTarget target = parse_target(request.target);

  if (is_get(request) && target.path == "/v1/health") {
    return json_response(200, nlohmann::json{{"status", "ok"},
                                             {"protocol", 1},
                                             {"server_time_ms", now_millis()},
                                             {"min_retained_seq", 0}});
  }

  if (is_post(request) && target.path == "/v1/enroll") {
    auto body = nlohmann::json::parse(request.body, nullptr, false);
    if (body.is_discarded()) {
      return error_response(ErrorCode::BadRequest, "invalid JSON body");
    }
    try {
      const auto enrollment = auth_.enroll(body.at("enrollment_token").get<std::string>(),
                                           body.at("device_name").get<std::string>(),
                                           body.at("platform").get<std::string>(),
                                           body.value("app_version", std::string{}));
      if (!enrollment) {
        return error_response(enrollment.error());
      }
      return json_response(201, nlohmann::json{{"device_id", enrollment.value().device_id},
                                               {"device_token", enrollment.value().device_token},
                                               {"space_id", enrollment.value().space_id},
                                               {"space_name", enrollment.value().space_name},
                                               {"current_seq", enrollment.value().current_seq.value},
                                               {"protocol", 1}});
    } catch (const std::exception& exception) {
      return error_response(ErrorCode::BadRequest, exception.what());
    }
  }

  if (is_get(request) && target.path == "/v1/me") {
    const auto token = bearer_token(request);
    if (!token.has_value()) {
      return error_response(ErrorCode::AuthRequired, "missing bearer token");
    }
    auto device = auth_.authenticate(*token);
    if (!device) {
      return error_response(device.error());
    }
    auto latest = store_.latest_seq(device.value().space_id);
    if (!latest) {
      return error_response(latest.error());
    }
    return json_response(200, nlohmann::json{{"device_id", device.value().device_id},
                                             {"space_id", device.value().space_id},
                                             {"display_name", device.value().display_name},
                                             {"platform", device.value().platform},
                                             {"status", device.value().status},
                                             {"last_ack_seq", device.value().last_ack_seq.value},
                                             {"server_seq", latest.value().value}});
  }

  // /v1/spaces/{space_id}/...
  if (target.segments.size() >= 4 && target.segments[0] == "v1" &&
      target.segments[1] == "spaces") {
    const std::string& space_id = target.segments[2];
    const std::string& action = target.segments[3];

    const auto token = bearer_token(request);
    if (!token.has_value()) {
      return error_response(ErrorCode::AuthRequired, "missing bearer token");
    }
    auto device = auth_.authenticate(*token);
    if (!device) {
      return error_response(device.error());
    }
    if (device.value().space_id != space_id) {
      return error_response(ErrorCode::SpaceForbidden, "device does not belong to this space");
    }

    if (is_post(request) && action == "ops" && target.segments.size() == 4) {
      auto body = nlohmann::json::parse(request.body, nullptr, false);
      if (body.is_discarded()) {
        return error_response(ErrorCode::BadRequest, "invalid JSON body");
      }
      try {
        const std::string kind_text = body.at("kind").get<std::string>();
        auto kind = op_kind_from_string(kind_text);
        if (!kind.has_value()) {
          return error_response(ErrorCode::BadRequest, "invalid operation kind");
        }

        CommitRequest commit;
        commit.space_id = space_id;
        commit.device_id = device.value().device_id;
        commit.op_id = OpId{body.at("op_id").get<std::string>()};
        commit.kind = *kind;
        if (body.contains("file_id") && !body["file_id"].is_null()) {
          commit.file_id = FileId{body["file_id"].get<std::string>()};
        }
        commit.base_revision = Revision{body.value("base_revision", 0LL)};
        commit.path = body.at("path").get<std::string>();
        if (body.contains("content_hash") && !body["content_hash"].is_null()) {
          auto hash = ContentHash::from_hex(body["content_hash"].get<std::string>());
          if (!hash.has_value()) {
            return error_response(ErrorCode::BadRequest, "invalid content_hash");
          }
          commit.content_hash = *hash;
        }
        if (body.contains("size") && !body["size"].is_null()) {
          commit.size = body["size"].get<std::int64_t>();
        }
        commit.mtime_ms = body.value("mtime_ms", 0LL);
        commit.client_seq = Seq{body.value("client_seq", 0LL)};
        commit.conflict = body.value("conflict", false);

        auto request_hash = sha256(body.dump());
        if (!request_hash) {
          return error_response(request_hash.error());
        }
        commit.request_hash = request_hash.value().to_hex();

        auto outcome = ops_.commit(commit);
        if (auto* success = std::get_if<CommitSuccess>(&outcome)) {
          auto response = json_response(200, success->response);
          if (success->replayed) {
            response.headers.emplace_back("X-Idempotent-Replay", "true");
          }
          return response;
        }
        const auto& failure = std::get<CommitFailure>(outcome);
        return error_response(failure.code, failure.message, failure.details);
      } catch (const std::exception& exception) {
        return error_response(ErrorCode::BadRequest, exception.what());
      }
    }

    if (is_get(request) && action == "changes" && target.segments.size() == 4) {
      const auto since_it = target.query.find("since");
      if (since_it == target.query.end()) {
        return error_response(ErrorCode::BadRequest, "since is required");
      }
      const auto since = parse_int64(since_it->second);
      if (!since.has_value() || *since < 0) {
        return error_response(ErrorCode::BadRequest, "invalid since value");
      }
      int limit = 200;
      if (const auto it = target.query.find("limit"); it != target.query.end()) {
        const auto parsed = parse_int64(it->second);
        if (!parsed.has_value() || *parsed <= 0) {
          return error_response(ErrorCode::BadRequest, "invalid limit");
        }
        limit = static_cast<int>(std::min<std::int64_t>(*parsed, 1000));
      }

      auto changes = store_.list_changes(space_id, Seq{*since}, limit);
      if (!changes) {
        return error_response(changes.error());
      }

      nlohmann::json out;
      out["space_id"] = space_id;
      out["from_seq"] = *since + 1;
      out["to_seq"] = changes.value().empty() ? *since : changes.value().back().seq.value;
      out["has_more"] = static_cast<int>(changes.value().size()) == limit;
      out["changes"] = nlohmann::json::array();
      for (const auto& change : changes.value()) {
        out["changes"].push_back(change_json(change));
      }
      return json_response(200, out);
    }

    if (is_get(request) && action == "snapshot" && target.segments.size() == 4) {
      int limit = 500;
      if (const auto it = target.query.find("limit"); it != target.query.end()) {
        const auto parsed = parse_int64(it->second);
        if (!parsed.has_value() || *parsed <= 0) {
          return error_response(ErrorCode::BadRequest, "invalid limit");
        }
        limit = static_cast<int>(std::min<std::int64_t>(*parsed, 2000));
      }
      std::string page_token;
      if (const auto it = target.query.find("page_token"); it != target.query.end()) {
        page_token = it->second;
      }

      auto rows = store_.list_snapshot(space_id, page_token, limit);
      if (!rows) {
        return error_response(rows.error());
      }
      auto latest = store_.latest_seq(space_id);
      if (!latest) {
        return error_response(latest.error());
      }

      nlohmann::json out;
      out["space_id"] = space_id;
      out["snapshot_seq"] = latest.value().value;
      out["entries"] = nlohmann::json::array();
      for (const auto& entry : rows.value()) {
        out["entries"].push_back(entry_snapshot_json(entry));
      }
      if (static_cast<int>(rows.value().size()) == limit) {
        const auto& last = rows.value().back();
        out["next_page_token"] = last.path_key + std::string(1, '\x1f') + last.file_id.value;
      } else {
        out["next_page_token"] = nullptr;
      }
      return json_response(200, out);
    }

    if (is_get(request) && action == "entries" && target.segments.size() == 5) {
      auto entry = store_.find_entry(space_id, FileId{target.segments[4]});
      if (!entry) {
        return error_response(entry.error());
      }
      if (!entry.value().has_value()) {
        return error_response(ErrorCode::FileNotFound, "file not found");
      }
      return json_response(200, entry_snapshot_json(*entry.value()));
    }
  }

  // /v1/devices/{device_id}/ack
  if (is_post(request) && target.segments.size() == 4 && target.segments[0] == "v1" &&
      target.segments[1] == "devices" && target.segments[3] == "ack") {
    const auto token = bearer_token(request);
    if (!token.has_value()) {
      return error_response(ErrorCode::AuthRequired, "missing bearer token");
    }
    auto device = auth_.authenticate(*token);
    if (!device) {
      return error_response(device.error());
    }
    if (device.value().device_id != target.segments[2]) {
      return error_response(ErrorCode::SpaceForbidden, "cannot ack for another device");
    }
    auto body = nlohmann::json::parse(request.body, nullptr, false);
    if (body.is_discarded() || !body.contains("last_seq")) {
      return error_response(ErrorCode::BadRequest, "last_seq is required");
    }
    auto updated = store_.update_device_ack(device.value().device_id,
                                            Seq{body.value("last_seq", 0LL)});
    if (!updated) {
      return error_response(updated.error());
    }
    net::HttpResponse response;
    response.status = 204;
    response.body.clear();
    return response;
  }

  return error_response(ErrorCode::NotFound, "route not found");
}

}  // namespace syncbox::server
