#ifndef WEBPP11_SERVER_H
#define WEBPP11_SERVER_H

#include <boost/asio.hpp>
#include <regex>
#include <thread>

#include "webpp11/logger.h"
#include "webpp11/request.h"
#include "webpp11/response.h"

namespace webpp {

typedef boost::asio::ip::tcp::socket HTTP;

typedef std::map<
    std::string,
    std::unordered_map<std::string,
                       std::function<std::shared_ptr<Response>(Request&)>>>
    Routes;

class Server {
 public:
  Server(unsigned short port, size_t num_threads = 1)
      : endpoint(boost::asio::ip::tcp::v4(), port),
        acceptor(io_service, endpoint),
        num_threads(num_threads),
        logger(new Logger()) {}

  void start(Routes& routes) {
    for (auto it = routes.begin(); it != routes.end(); ++it) {
      all_routes.push_back(it);
    }

    accept();

    for (size_t i = 1; i < num_threads; ++i) {
      threads.emplace_back([this]() { io_service.run(); });
    }

    io_service.run();

    for (auto& t : threads) {
      t.join();
    }
  }

 private:
  std::vector<Routes::iterator> all_routes;

  boost::asio::io_service io_service;
  boost::asio::ip::tcp::endpoint endpoint;
  boost::asio::ip::tcp::acceptor acceptor;

  std::shared_ptr<Logger> logger;

  size_t num_threads;
  std::vector<std::thread> threads;

  void accept() {
    auto socket = std::make_shared<HTTP>(io_service);

    acceptor.async_accept(*socket,
                          [this, socket](const boost::system::error_code& e) {
                            //                            accept();

                            if (!e) process(socket);
                          });
  }

  void process(std::shared_ptr<HTTP> socket) const {
    auto read_buffer = std::make_shared<boost::asio::streambuf>();

    boost::asio::async_read_until(
        *socket, *read_buffer, "\r\n\r\n",
        [this, socket, read_buffer](const boost::system::error_code& e,
                                    size_t bytes_transferred) {
          if (!e) {
            size_t total = read_buffer->size();

            std::istream stream(read_buffer.get());

            auto request = std::make_shared<Request>();
            *request = parse_request(stream);

            size_t num_additional_bytes = total - bytes_transferred;

            if (request->header.count("Content-Length") > 0) {
              boost::asio::async_read(
                  *socket, *read_buffer,
                  boost::asio::transfer_exactly(
                      std::stoull(request->header["Content-Length"]) -
                      num_additional_bytes),
                  [this, socket, read_buffer, request](
                      const boost::system::error_code& e,
                      size_t byte_transferred) {
                    if (!e) {
                      request->content = std::shared_ptr<std::istream>(
                          new std::istream(read_buffer.get()));
                      respond(socket, request);
                    }
                  });
            } else {
              respond(socket, request);
            }
          }
        });
  }

  void respond(std::shared_ptr<HTTP> socket,
               std::shared_ptr<Request> request) const {
    for (auto res_it : all_routes) {
      if (res_it->first == request->path) {
        if (res_it->second.count(request->method) > 0) {
          auto response = res_it->second[request->method](*request);
          auto write_buffer = response->get_buffer();

          logger->info(request->path + " " + request->method + ": " +
                       http::HttpStatusMap[response->get_status()]);

          boost::asio::async_write(
              *socket, *write_buffer,
              [this, socket, request, write_buffer](
                  const boost::system::error_code& ec,
                  size_t bytes_transferred) {
                if (!ec && std::stof(request->http_version) > 1.05)
                  process(socket);
              });
          return;
        }
      }
    }

    auto not_found_res = std::make_shared<Response>("", http::NotFound);
    auto write_buffer = not_found_res->get_buffer();

    logger->info(request->path + " " + request->method + ": " +
                 http::HttpStatusMap[not_found_res->get_status()]);

    boost::asio::async_write(
        *socket, *write_buffer,
        [this, socket, request, write_buffer](
            const boost::system::error_code& e, size_t bytes_transferred) {
          if (!e && std::stof(request->http_version) > 1.05) {
            process(socket);
          }
        });
  }

  Request parse_request(std::istream& stream) const {
    Request request;
    std::unordered_map<std::string, std::string> header;

    std::regex e("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    std::smatch sub_match;

    std::string line;
    getline(stream, line);
    line.pop_back();

    if (std::regex_match(line, sub_match, e)) {
      request.method = sub_match[1];
      request.path = sub_match[2];
      request.http_version = sub_match[3];

      bool matched;
      e = "^([^:]*): ?(.*)$";
      do {
        getline(stream, line);
        line.pop_back();
        matched = std::regex_match(line, sub_match, e);
        if (matched) {
          request.header[sub_match[1]] = sub_match[2];
        }
      } while (matched);
    }

    return request;
  }
};
};  // namespace webpp

#endif