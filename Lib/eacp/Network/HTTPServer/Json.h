#pragma once

#include <eacp/Network/Network.h>

#include <Miro/Json.h>
#include <Miro/Reflect.h>

namespace eacp::HTTP::Json
{

struct EmptyRequest
{
    MIRO_REFLECT()
};

struct ErrorResponse
{
    std::string error;
    int status = 500;

    MIRO_REFLECT(error, status)
};

template <typename T>
void setJson(Response& response, const T& value)
{
    response.setContent(Miro::toJSONString(value), "application/json");
}

[[noreturn]] inline void throwError(const std::string& message, int statusCode = 400)
{
    auto body = ErrorResponse {.error = message, .status = statusCode};
    auto response = Response();
    response.statusCode = statusCode;
    setJson(response, body);
    throw Error(std::move(response));
}

template <typename Resp, typename Req>
RequestHandler makeHandler(Resp (*fn)(const Req&))
{
    return [fn](const Request& req) -> Response
    {
        auto input = Req();
        try
        {
            // Miro::fromJSONString swallows malformed input (it never throws),
            // so parse explicitly — Json::parse still rejects bad JSON — and an
            // empty body stays an empty request rather than a 400.
            if (!req.body.empty())
                Miro::fromJSON(input, Miro::Json::parse(req.body));
        }
        catch (const std::exception& e)
        {
            throwError(std::string("Invalid request body: ") + e.what());
        }

        auto result = fn(input);
        auto response = Response();
        response.statusCode = 200;
        setJson(response, result);
        return response;
    };
}

struct Server : eacp::HTTP::Server
{
    using eacp::HTTP::Server::del;
    using eacp::HTTP::Server::get;
    using eacp::HTTP::Server::post;
    using eacp::HTTP::Server::put;
    using eacp::HTTP::Server::Server;

    template <typename Resp, typename Req>
    void post(const std::string& path, Resp (*fn)(const Req&))
    {
        eacp::HTTP::Server::post(path, makeHandler(fn));
    }

    template <typename Resp, typename Req>
    void get(const std::string& path, Resp (*fn)(const Req&))
    {
        eacp::HTTP::Server::get(path, makeHandler(fn));
    }

    template <typename Resp, typename Req>
    void put(const std::string& path, Resp (*fn)(const Req&))
    {
        eacp::HTTP::Server::put(path, makeHandler(fn));
    }

    template <typename Resp, typename Req>
    void del(const std::string& path, Resp (*fn)(const Req&))
    {
        eacp::HTTP::Server::del(path, makeHandler(fn));
    }
};

} // namespace eacp::HTTP::Json
