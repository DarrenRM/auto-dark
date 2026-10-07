#include "http.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>

#include "log.h"

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string urlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < s.size()) {
      out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else out += s[i];
  }
  return out;
}

const char* statusText(int s) {
  switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    default: return "Error";
  }
}

bool sendAll(SOCKET s, const char* data, size_t len) {
  while (len > 0) {
    int n = send(s, data, (int)std::min<size_t>(len, 1 << 20), 0);
    if (n <= 0) return false;
    data += n;
    len -= n;
  }
  return true;
}

}  // namespace

bool HttpServer::start(int port, Handler handler) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  port_ = port;
  handler_ = std::move(handler);
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  BOOL excl = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (sockaddr*)&addr, sizeof addr) != 0 || listen(s, 16) != 0) {
    logf("http: cannot listen on 127.0.0.1:%d (error %d)", port, WSAGetLastError());
    closesocket(s);
    return false;
  }
  listen_ = (uintptr_t)s;
  running_ = true;
  thread_ = std::thread([this] { run(); });
  logf("http: control panel at http://127.0.0.1:%d/", port);
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) return;
  closesocket((SOCKET)listen_);
  if (thread_.joinable()) thread_.join();
  WSACleanup();
}

void HttpServer::run() {
  while (running_) {
    SOCKET c = accept((SOCKET)listen_, nullptr, nullptr);
    if (c == INVALID_SOCKET) {
      if (!running_) break;
      continue;
    }
    handleClient((uintptr_t)c);
    closesocket(c);
  }
}

void HttpServer::handleClient(uintptr_t sockv) {
  SOCKET s = (SOCKET)sockv;
  DWORD timeout = 2000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);

  std::string buf;
  char tmp[8192];
  size_t headerEnd = std::string::npos;
  while (headerEnd == std::string::npos) {
    int n = recv(s, tmp, sizeof tmp, 0);
    if (n <= 0) return;
    buf.append(tmp, n);
    headerEnd = buf.find("\r\n\r\n");
    if (buf.size() > 65536 && headerEnd == std::string::npos) return;
  }

  HttpRequest req;
  size_t lineEnd = buf.find("\r\n");
  std::string line = buf.substr(0, lineEnd);
  size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return;
  req.method = line.substr(0, sp1);
  std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t q = target.find('?');
  req.path = urlDecode(target.substr(0, q));
  if (q != std::string::npos) {
    std::string qs = target.substr(q + 1);
    size_t pos = 0;
    while (pos <= qs.size()) {
      size_t amp = qs.find('&', pos);
      std::string kv = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
      size_t eq = kv.find('=');
      if (!kv.empty()) req.query[urlDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : urlDecode(kv.substr(eq + 1));
      if (amp == std::string::npos) break;
      pos = amp + 1;
    }
  }
  size_t pos = lineEnd + 2;
  while (pos < headerEnd) {
    size_t e = buf.find("\r\n", pos);
    std::string h = buf.substr(pos, e - pos);
    size_t colon = h.find(':');
    if (colon != std::string::npos) {
      std::string v = h.substr(colon + 1);
      v.erase(0, v.find_first_not_of(" \t"));
      req.headers[lower(h.substr(0, colon))] = v;
    }
    pos = e + 2;
  }

  HttpResponse res;
  // Reject requests that don't come from our own page: blocks DNS rebinding (Host) and
  // cross-site form/fetch posts from other websites (Origin, JSON content type).
  std::string host = lower(req.headers["host"]);
  std::string p = std::to_string(port_);
  bool hostOk = host == "127.0.0.1:" + p || host == "localhost:" + p;
  std::string origin = lower(req.headers["origin"]);
  bool originOk = origin.empty() || origin == "http://127.0.0.1:" + p || origin == "http://localhost:" + p;
  size_t len = req.headers.count("content-length") ? (size_t)strtoull(req.headers["content-length"].c_str(), nullptr, 10) : 0;

  if (!hostOk || !originOk) {
    res.status = 403;
    res.body = "{\"error\":\"forbidden\"}";
  } else if (req.method == "POST" && lower(req.headers["content-type"]).rfind("application/json", 0) != 0) {
    res.status = 403;
    res.body = "{\"error\":\"POST requires Content-Type: application/json\"}";
  } else if (len > (1 << 20)) {
    res.status = 413;
    res.body = "{\"error\":\"too large\"}";
  } else {
    req.body = buf.substr(headerEnd + 4);
    while (req.body.size() < len) {
      int n = recv(s, tmp, sizeof tmp, 0);
      if (n <= 0) return;
      req.body.append(tmp, n);
    }
    req.body.resize(std::min(req.body.size(), len));
    try {
      res = handler_(req);
    } catch (std::exception& e) {
      res.status = 500;
      res.body = std::string("{\"error\":\"") + e.what() + "\"}";
    }
  }

  std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " + statusText(res.status) +
                     "\r\nContent-Type: " + res.contentType + "\r\nContent-Length: " + std::to_string(res.body.size()) +
                     "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  if (sendAll(s, head.data(), head.size())) sendAll(s, res.body.data(), res.body.size());
  shutdown(s, SD_SEND);
}
