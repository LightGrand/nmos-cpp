#ifndef NMOS_NODE_API_H
#define NMOS_NODE_API_H

#include <mutex>
#include "cpprest/api_router.h"
#include "nmos/resources.h"

namespace slog
{
    class base_gate;
}

// Node API implementation
// See https://github.com/AMWA-TV/nmos-discovery-registration/blob/v1.2-dev/APIs/NodeAPI.raml
namespace nmos
{
    web::http::experimental::listener::api_router make_node_api(nmos::resources& resources, std::mutex& mutex, slog::base_gate& gate);
}

#endif
