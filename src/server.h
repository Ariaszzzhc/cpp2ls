#ifndef CPP2LS_SERVER_H
#define CPP2LS_SERVER_H

#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>

namespace cpp2ls {
  using headers_t = std::unordered_map<std::string, std::string>;

  struct request {
    headers_t headers;
    nlohmann::json body;
  };

  struct response {
    headers_t headers;
    nlohmann::json body;
  };

  class server {
  public:
    server(std::istream& input, std::ostream& output);

    void run();

  private:
    std::optional<request> read_request();

    response handle_request(const request& req);

    void send_response(const response& res);

  private:
    std::istream* m_input_stream;
    std::ostream* m_output_stream;
  };
}  // namespace cpp2ls

#endif  // !CPP2LS_SERVER_H
