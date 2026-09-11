#if defined(SURFACE_STICK_S3)
#include "WriterServer.h"
#include "WriterPage.h"
#include <sys/socket.h>
#include <cerrno>

namespace surface::device {
void WriterServer::reply(int code, const std::string& type, const std::string& body) {
  output = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK\r\n" : " Error\r\n") +
           "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
           "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src "
           "'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; "
           "form-action 'none'\r\n"
           "Content-Type: " +
           type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  sent = 0;
}
bool WriterServer::poll(CardWriter& writer, uint64_t now) {
  bool activity = false;
  if (WiFi.status() != WL_CONNECTED) {
    stop();
    return false;
  }
  const std::string address = WiFi.localIP().toString().c_str();
  if (started && address != ip)
    stop();
  if (!started) {
    ip = address;
    server.begin();
    started = true;
    Serial.printf("[writer] URL=http://%s/\n", ip.c_str());
  }
  if (!client) {
    client = server.accept();
    if (!client)
      return false;
    client.setTimeout(0);
    request = WriterHttpRequest{};
    output.clear();
    sent = 0;
    accepted = millis();
  }
  if (millis() - accepted > 3000) {
    client.stop();
    return false;
  }
  if (output.empty()) {
    unsigned budget = 512;
    while (budget-- && client.available() && !request.complete && !request.error)
      request.feed(char(client.read()));
    if (request.error)
      reply(request.error, "application/json", "{\"error\":\"Invalid or oversized HTTP request\"}");
    else if (request.complete) {
      if (!request.sameOrigin(ip))
        reply(
            403, "application/json",
            "{\"error\":\"Open this page using the Stick IP address; same-origin JSON required\"}");
      else if (request.path == "/" && request.method == "GET") {
        reply(200, "text/html; charset=utf-8", writerPage);
        activity = true;
      } else {
        std::string body;
        int code = writer.request(request.method, request.path, request.body, now, body);
        reply(code, "application/json", body);
      }
    }
  }
  if (!output.empty()) {
    // Never wait for a slow phone while buttons/NFC need sampling.
    const size_t count = std::min<size_t>(512, output.size() - sent);
    const int written = ::send(client.fd(), output.data() + sent, count, MSG_DONTWAIT);
    if (written > 0)
      sent += size_t(written);
    else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      client.stop();
      output.clear();
      return activity;
    }
    if (sent == output.size()) {
      client.stop();
      output.clear();
    }
  }
  return activity;
}
void WriterServer::stop() {
  client.stop();
  if (started)
    server.end();
  started = false;
  ip.clear();
  output.clear();
}
} // namespace surface::device
#endif
