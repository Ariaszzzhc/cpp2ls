#include "server.h"

#include <format>
#include <optional>
#include <ostream>
#include <string>

namespace cpp2ls {
  void trim_str(std::string& str) {
    std::string WHITESPACE{" \n\r\t\f\v"};
    auto end = str.find_last_not_of(WHITESPACE);
    auto start = str.find_first_not_of(WHITESPACE);

    if (end == std::string::npos) {
      str = "";
    } else {
      str = str.substr(start, end - start + 1);
    }
  }

  server::server(std::istream& input, std::ostream& output)
      : m_input_stream{&input}, m_output_stream{&output} {}

  void server::run() {
    while (m_input_stream->good()) {
      try {
        auto req = read_request();

        if (req.has_value()) {
          auto res = handle_request(*req);

          send_response(res);
        }
      } catch (const nlohmann::json::parse_error& err) {
        headers_t headers;
        nlohmann::json res_body;
        nlohmann::json error;
        error["code"] = -32700;
        error["message"] = "Parse error";

        res_body["jsonrpc"] = "2.0";
        res_body["error"] = error;
        res_body["id"] = nullptr;
        auto raw_body = res_body.dump();
        headers["Content-Length"] = std::to_string(raw_body.size());

        response res{headers, res_body};
        send_response(res);
      } catch (const std::exception& err) {
        std::cerr << std::format("Error: {}\n", err.what());
      }
    }
  }

  std::optional<request> server::read_request() {
    headers_t headers;
    std::string line;
    while (std::getline(*m_input_stream, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.empty()) {
        break;
      }

      auto colon_pos = line.find(':');
      if (colon_pos != std::string::npos) {
        auto key = line.substr(0, colon_pos);
        auto value = line.substr(colon_pos + 1);
        // Trim whitespace
        trim_str(key);
        trim_str(value);
        headers[key] = value;
      }
    }

    if (headers.find("Content-Length") != headers.end()) {
      auto content_length = std::stoi(headers["Content-Length"]);
      std::string raw_body(content_length, '\0');
      m_input_stream->read(&raw_body[0], content_length);
      auto body = nlohmann::json::parse(raw_body);

      return request{headers, body};
    }

    return std::nullopt;
  }

  void server::send_response(const response& res) {
    for (const auto& [key, value] : res.headers) {
      *m_output_stream << std::format("{}: {}\r\n", key, value);
    }
    *m_output_stream << "\r\n";
    *m_output_stream << res.body.dump();
    m_output_stream->flush();
  }

  response server::handle_request(const request& req) {
    auto id = req.body["id"].get<std::int32_t>();
    auto method = req.body["method"].get<std::string>();

    response res;

    return res;
  }

}  // namespace cpp2ls
