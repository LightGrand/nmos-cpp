#include "nmos/logging_api.h"

#include "nmos/api_utils.h"
#include "nmos/query_utils.h"
#include "rql/rql.h"

namespace nmos
{
    namespace experimental
    {
        web::http::experimental::listener::api_router make_logging_api(nmos::experimental::log_model& model, std::mutex& mutex, slog::base_gate& gate)
        {
            using namespace web::http::experimental::listener::api_router_using_declarations;

            api_router logging_api;

            logging_api.support(U("/?"), methods::GET, [](const http_request&, http_response& res, const string_t&, const route_parameters&)
            {
                set_reply(res, status_codes::OK, value_of({ JU("log/") }));
                return true;
            });

            logging_api.support(U("/log/?"), methods::GET, [](const http_request&, http_response& res, const string_t&, const route_parameters&)
            {
                set_reply(res, status_codes::OK, value_of({ JU("events/") }));
                return true;
            });

            logging_api.support(U("/log/events/?"), methods::GET, [&model, &mutex, &gate](const http_request& req, http_response& res, const string_t&, const route_parameters& parameters)
            {
                std::lock_guard<std::mutex> lock(mutex);

                auto flat_query_params = web::json::value_from_query(req.request_uri().query());
                for (auto& param : flat_query_params.as_object())
                {
                    // special case, RQL needs the URI-encoded string
                    if (U("query.rql") == param.first) continue;
                    // everything else needs the decoded string
                    param.second = web::json::value::string(web::uri::decode(param.second.as_string()));
                }

                value basic_query = web::json::unflatten(flat_query_params);
                value rql_query;

                const bool paging = basic_query.has_field(U("paging"));
                const size_t offset = paging ? nmos::fields::offset(basic_query.at(U("paging"))) : 0;
                const size_t limit = paging ? nmos::fields::limit(basic_query.at(U("paging"))) : (std::numeric_limits<size_t>::max)();
                if (basic_query.has_field(U("paging")))
                {
                    basic_query.erase(U("paging"));
                }
                if (basic_query.has_field(U("query")))
                {
                    auto& advanced = basic_query.at(U("query"));
                    if (advanced.has_field(U("rql")))
                    {
                        rql_query = rql::parse_query(web::json::field_as_string{ U("rql") }(advanced));
                    }
                    basic_query.erase(U("query"));
                }

                auto paged = nmos::paged<event>(
                    [&basic_query, &rql_query](const event& event)
                    {
                        return web::json::match_query(event.data, basic_query, web::json::match_icase | web::json::match_substr)
                            && rql_query.is_null() || rql::evaluator
                            {
                                [&event](web::json::value& results, const web::json::value& key)
                                {
                                    return web::json::extract(event.data.as_object(), results, key.as_string());
                                },
                                rql::default_any_operators()
                            }(rql_query) == web::json::value::boolean(true);
                    },
                    offset, limit);
                size_t& count = paged.count;

                set_reply(res, status_codes::OK,
                    web::json::serialize_if(model.events,
                        std::ref(paged), // to ensure count is updated
                        [](const event& event){ return event.data; }),
                    U("application/json"));
                res.headers().add(U("X-Total-Count"), count);

                slog::log<slog::severities::too_much_info>(gate, SLOG_FLF) << nmos::api_stash(req, parameters) << "Returning " << (count < offset ? 0 : count < offset + limit ? count - offset : limit) << " matching log events";

                return true;
            });

            logging_api.support(U("/log/events/?"), methods::DEL, [&model, &mutex](const http_request& req, http_response& res, const string_t&, const route_parameters&)
            {
                std::lock_guard<std::mutex> lock(mutex);

                if (req.request_uri().query().empty())
                {
                    model.events.clear();
                    set_reply(res, status_codes::NoContent);
                }
                else
                {
                    set_reply(res, status_codes::NotImplemented);
                }

                return true;
            });

            logging_api.support(U("/log/events/") + nmos::patterns::resourceId.pattern + U("/?"), methods::GET, [&model, &mutex](const http_request&, http_response& res, const string_t&, const route_parameters& parameters)
            {
                std::lock_guard<std::mutex> lock(mutex);

                const string_t eventId = parameters.at(nmos::patterns::resourceId.name);

                typedef events::index<tags::id>::type by_id;
                by_id& ids = model.events.get<tags::id>();

                auto event = ids.find(eventId);
                if (ids.end() != event)
                {
                    set_reply(res, status_codes::OK, event->data);
                }
                else
                {
                    set_reply(res, status_codes::NotFound);
                }

                return true;
            });

            nmos::add_api_finally_handler(logging_api, gate);

            return logging_api;
        }

        namespace detail
        {
            inline web::json::value json_from_string(const std::string& s)
            {
                return web::json::value{ utility::s2us(s) };
            }

            template <typename T>
            inline std::string ostringstreamed(const T& value)
            {
                return [&]{ std::ostringstream os; os << value; return os.str(); }();
            }

            inline web::json::value json_from_message(const slog::async_log_message& message)
            {
                web::json::value json_source_location = web::json::value::object(true);
                json_source_location[U("file")] = json_from_string(message.file());
                json_source_location[U("line")] = message.line();
                json_source_location[U("function")] = json_from_string(message.function());

                web::json::value json_message = web::json::value::object(true);
                json_message[U("timestamp")] = json_from_string(ostringstreamed(slog::put_timestamp(message.timestamp(), "%Y-%m-%dT%H:%M:%06.3SZ")));
                json_message[U("level")] = message.level();
                json_message[U("level_name")] = json_from_string(ostringstreamed(slog::put_severity_name(message.level())));
                json_message[U("thread_id")] = json_from_string(ostringstreamed(message.thread_id()));
                json_message[U("source_location")] = json_source_location;
                json_message[U("message")] = json_from_string(message.str());

                const auto http_method = nmos::get_http_method_stash(message.stream());
                if (!http_method.empty()) json_message[U("http_method")] = web::json::value::string(http_method);
                const auto request_uri = nmos::get_request_uri_stash(message.stream());
                if (!request_uri.is_empty()) json_message[U("request_uri")] = web::json::value::string(request_uri.to_string());
                const auto route_parameters = nmos::get_route_parameters_stash(message.stream());
                if (!route_parameters.empty()) json_message[U("route_parameters")] = web::json::value_from_fields(route_parameters);

                // adding an id just to allow the API to provide access to single events in the standard REST manner
                json_message[U("id")] = web::json::value::string(nmos::make_id());
                return json_message;
            }
        }

        void log_to_model(log_model& model, const slog::async_log_message& message)
        {
            // capacity ought to be part of log/settings
            const std::size_t capacity = 1234;
            while (model.events.size() > capacity)
            {
                model.events.pop_front();
            }
            model.events.push_back({ detail::json_from_message(message) });
        }
    }
}
