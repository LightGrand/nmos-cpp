#include "nmos/node_api.h"

#include "nmos/api_downgrade.h"
#include "nmos/api_utils.h"
#include "nmos/slog.h"
#include "cpprest/host_utils.h"

namespace nmos
{
    web::http::experimental::listener::api_router make_unmounted_node_api(nmos::resources& resources, std::mutex & mutex, slog::base_gate& gate);

    web::http::experimental::listener::api_router make_node_api(nmos::resources& resources, std::mutex& mutex, slog::base_gate& gate)
    {
        using namespace web::http::experimental::listener::api_router_using_declarations;

        api_router node_api;

        node_api.support(U("/?"), methods::GET, [](const http_request&, http_response& res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("x-nmos/") }));
            return true;
        });

        node_api.support(U("/x-nmos/?"), methods::GET, [](const http_request&, http_response& res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("node/") }));
            return true;
        });

        node_api.support(U("/x-nmos/") + nmos::patterns::node_api.pattern + U("/?"), methods::GET, [](const http_request&, http_response& res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("v1.0/"), JU("v1.1/"), JU("v1.2/") }));
            return true;
        });

        node_api.mount(U("/x-nmos/") + nmos::patterns::node_api.pattern + U("/") + nmos::patterns::is04_version.pattern, make_unmounted_node_api(resources, mutex, gate));

        nmos::add_api_finally_handler(node_api, gate);

        return node_api;
    }

    web::http::experimental::listener::api_router make_unmounted_node_api(nmos::resources& resources, std::mutex& mutex, slog::base_gate& gate)
    {
        using namespace web::http::experimental::listener::api_router_using_declarations;

        api_router node_api;

        node_api.support(U("/?"), methods::GET, [](const http_request&, http_response& res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("self/"), JU("devices/"), JU("sources/"), JU("flows/"), JU("senders/"), JU("receivers/") }));
            return true;
        });

        node_api.support(U("/self/?"), methods::GET, [&resources, &mutex, &gate](const http_request& req, http_response& res, const string_t&, const route_parameters& parameters)
        {
            std::lock_guard<std::mutex> lock(mutex);

            const nmos::api_version version = nmos::parse_api_version(parameters.at(nmos::patterns::is04_version.name));

            auto resource = std::find_if(resources.begin(), resources.end(), [](const nmos::resources::value_type& resource){ return nmos::types::node == resource.type; });
            if (resources.end() != resource && nmos::is_permitted_downgrade(*resource, version))
            {
                slog::log<slog::severities::more_info>(gate, SLOG_FLF) << nmos::api_stash(req, parameters) << "Returning self resource: " << resource->id;
                set_reply(res, status_codes::OK, nmos::downgrade(*resource, version));
            }
            else
            {
                slog::log<slog::severities::error>(gate, SLOG_FLF) << nmos::api_stash(req, parameters) << "Self resource not found!";
                set_reply(res, status_codes::NotFound);
            }

            return true;
        });

        node_api.support(U("/") + nmos::patterns::subresourceType.pattern + U("/?"), methods::GET, [&resources, &mutex, &gate](const http_request& req, http_response& res, const string_t&, const route_parameters& parameters)
        {
            std::lock_guard<std::mutex> lock(mutex);

            const nmos::api_version version = nmos::parse_api_version(parameters.at(nmos::patterns::is04_version.name));
            const string_t resourceType = parameters.at(nmos::patterns::subresourceType.name);

            const auto match = [&](const nmos::resources::value_type& resource) { return resource.type == nmos::type_from_resourceType(resourceType) && nmos::is_permitted_downgrade(resource, version); };

            size_t count = 0;

            set_reply(res, status_codes::OK,
                web::json::serialize_if(resources,
                    match,
                    [&count, &version](const nmos::resources::value_type& resource) { ++count; return nmos::downgrade(resource, version); }),
                U("application/json"));

            slog::log<slog::severities::info>(gate, SLOG_FLF) << nmos::api_stash(req, parameters) << "Returning " << count << " matching " << resourceType;

            return true;
        });

        node_api.support(U("/") + nmos::patterns::subresourceType.pattern + U("/") + nmos::patterns::resourceId.pattern + U("/?"), methods::GET, [&resources, &mutex, &gate](const http_request& req, http_response& res, const string_t&, const route_parameters& parameters)
        {
            std::lock_guard<std::mutex> lock(mutex);

            const nmos::api_version version = nmos::parse_api_version(parameters.at(nmos::patterns::is04_version.name));
            const string_t resourceType = parameters.at(nmos::patterns::subresourceType.name);
            const string_t resourceId = parameters.at(nmos::patterns::resourceId.name);

            auto resource = resources.find(resourceId);
            if (resources.end() != resource)
            {
                if (resource->type == nmos::type_from_resourceType(resourceType) && nmos::is_permitted_downgrade(*resource, version))
                {
                    slog::log<slog::severities::more_info>(gate, SLOG_FLF) << nmos::api_stash(req, parameters) << "Returning resource: " << resourceId;
                    set_reply(res, status_codes::OK, nmos::downgrade(*resource, version));
                }
                else
                {
                    set_reply(res, status_codes::NotFound);
                }
            }
            else
            {
                set_reply(res, status_codes::NotFound);
            }

            return true;
        });

        node_api.support(U("/receivers/") + nmos::patterns::resourceId.pattern + U("/target"), methods::GET, [&resources, &mutex, &gate](const http_request& req, http_response& res, const string_t&, const route_parameters& parameters)
        {
            std::lock_guard<std::mutex> lock(mutex);

            const nmos::api_version version = nmos::parse_api_version(parameters.at(nmos::patterns::is04_version.name));
            const string_t resourceId = parameters.at(nmos::patterns::resourceId.name);

            auto resource = resources.find(resourceId);
            if (resources.end() != resource)
            {
                if (types::receiver == resource->type && nmos::is_permitted_downgrade(*resource, version))
                {
                    set_reply(res, status_codes::NotImplemented);
                }
                else
                {
                    set_reply(res, status_codes::NotFound);
                }
            }
            else
            {
                set_reply(res, status_codes::NotFound);
            }

            return true;
        });

        return node_api;
    }
}
