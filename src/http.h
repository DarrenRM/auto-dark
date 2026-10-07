// Tiny blocking HTTP/1.1 server bound to 127.0.0.1 (one request per connection).
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

struct HttpRequest {
  std::string method, path, body;
  std::map<std::string, std::string> query, headers;  // header names lower-cased
};

struct HttpResponse {
  int status = 200;
  std::string contentType = "application/json";
  std::string body;
};

class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;
  ~HttpServer() { stop(); }
  bool start(int port, Handler handler);
  void stop();

 private:
  void run();
  void handleClient(uintptr_t sock);
  int port_ = 0;
  Handler handler_;
  uintptr_t listen_ = ~(uintptr_t)0;
  std::atomic<bool> running_{false};
  std::thread thread_;
};
