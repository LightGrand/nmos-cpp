#include "cpprest/host_utils.h"
#include "cpprest/ws_listener.h"
#include "nmos/api_utils.h"
#include "nmos/admin_ui.h"
#include "nmos/connection_api.h"
#include "nmos/logging_api.h"
#include "nmos/mdns_api.h"
#include "nmos/node_api.h"
#include "nmos/query_api.h"
#include "nmos/query_ws_api.h"
#include "nmos/registration_api.h"
#include "nmos/settings_api.h"
#include "nmos/server_resources.h"
#include "mdns/service_advertiser.h"
#include "main_gate.h"

int main(int argc, char* argv[])
{
    // Construct our data models and mutexes to protect each of them
    // plus variables to signal when the server is stopping

    nmos::resources self_resources;
    std::mutex self_mutex;

    nmos::model nmos_model;
    std::mutex nmos_mutex;

    nmos::experimental::log_model log_model;
    std::mutex log_mutex;
    std::atomic<slog::severity> level = slog::severities::more_info;

    bool shutdown = false;

    // Logging should all go through this logging gateway
    main_gate gate(log_model, log_mutex, level);

    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Starting nmos-cpp registry";

    // Settings can be passed on the command-line, and changed dynamically by POST to /settings/all on the Settings API
    //
    // * "logging_level": integer value, between 40 (least verbose, only fatal messages) and -40 (most verbose)
    // * "allow_invalid_resources": boolean value, true (cope with out-of-order Ledger and LAWO registrations) or false (a little less lax)
    //
    // E.g.
    //
    // # nmos-cpp-registry.exe "{\"logging_level\":-40}"
    // # curl -H "Content-Type: application/json" http://localhost:3209/settings/all -d "{\"logging_level\":-40}"
    //
    // In either case, omitted settings will assume their defaults (invisibly, currently)

    if (argc > 1)
    {
        std::error_code error;
        nmos_model.settings = web::json::value::parse(utility::s2us(argv[1]), error);
        if (error || !nmos_model.settings.is_object())
        {
            nmos_model.settings = web::json::value::null();
            slog::log<slog::severities::error>(gate, SLOG_FLF) << "Bad command-line settings [" << error << "]";
        }
        else
        {
            // Logging level is a special case (see nmos/settings_api.h)
            level = nmos::fields::logging_level(nmos_model.settings);
        }
    }

    if (nmos_model.settings.is_null())
    {
        // Prepare initial settings (different than defaults)
        nmos_model.settings = web::json::value::object();
        nmos_model.settings[nmos::fields::logging_level] = web::json::value::number(level);
        nmos_model.settings[nmos::fields::allow_invalid_resources] = web::json::value::boolean(true);
        nmos_model.settings[nmos::fields::host_name] = web::json::value::string(web::http::experimental::host_name());
        nmos_model.settings[nmos::fields::host_address] = web::json::value::string(web::http::experimental::host_addresses(web::http::experimental::host_name())[0]);
    }

    // Configure the mDNS API

    web::http::experimental::listener::api_router mdns_api = nmos::experimental::make_mdns_api(nmos_mutex, level, gate);
    web::http::experimental::listener::http_listener mdns_listener(web::http::experimental::listener::make_listener_uri(nmos::experimental::fields::mdns_port(nmos_model.settings)));
    nmos::support_api(mdns_listener, mdns_api);

    // Configure the Settings API

    web::http::experimental::listener::api_router settings_api = nmos::experimental::make_settings_api(nmos_model.settings, nmos_mutex, level, gate);
    web::http::experimental::listener::http_listener settings_listener(web::http::experimental::listener::make_listener_uri(nmos::experimental::fields::settings_port(nmos_model.settings)));
    nmos::support_api(settings_listener, settings_api);

    // Configure the Logging API

    web::http::experimental::listener::api_router logging_api = nmos::experimental::make_logging_api(log_model, log_mutex, gate);
    web::http::experimental::listener::http_listener logging_listener(web::http::experimental::listener::make_listener_uri(nmos::experimental::fields::logging_port(nmos_model.settings)));
    nmos::support_api(logging_listener, logging_api);

    // Configure the Query API

    web::http::experimental::listener::api_router query_api = nmos::make_query_api(nmos_model, nmos_mutex, gate);
    web::http::experimental::listener::http_listener query_listener(web::http::experimental::listener::make_listener_uri(nmos::fields::query_port(nmos_model.settings)));
    nmos::support_api(query_listener, query_api);

    nmos::websockets nmos_websockets;

    std::condition_variable query_ws_events_condition; // associated with nmos_mutex

    web::websockets::experimental::listener::validate_handler query_ws_validate_handler = nmos::make_query_ws_validate_handler(nmos_model, nmos_mutex, gate);
    web::websockets::experimental::listener::open_handler query_ws_open_handler = nmos::make_query_ws_open_handler(nmos_model, nmos_websockets, nmos_mutex, query_ws_events_condition, gate);
    web::websockets::experimental::listener::close_handler query_ws_close_handler = nmos::make_query_ws_close_handler(nmos_model, nmos_websockets, nmos_mutex, gate);
    web::websockets::experimental::listener::websocket_listener query_ws_listener(nmos::fields::query_ws_port(nmos_model.settings), nmos::make_slog_logging_callback(gate));
    query_ws_listener.set_validate_handler(std::ref(query_ws_validate_handler));
    query_ws_listener.set_open_handler(std::ref(query_ws_open_handler));
    query_ws_listener.set_close_handler(std::ref(query_ws_close_handler));

    std::thread query_ws_events_sending([&] { nmos::send_query_ws_events_thread(query_ws_listener, nmos_model, nmos_websockets, nmos_mutex, query_ws_events_condition, shutdown, gate); });

    // Configure the Registration API

    web::http::experimental::listener::api_router registration_api = nmos::make_registration_api(nmos_model, nmos_mutex, query_ws_events_condition, gate);
    web::http::experimental::listener::http_listener registration_listener(web::http::experimental::listener::make_listener_uri(nmos::fields::registration_port(nmos_model.settings)));
    nmos::support_api(registration_listener, registration_api);

    std::condition_variable registration_expiration_condition; // associated with nmos_mutex
    std::thread registration_expiration([&] { nmos::erase_expired_resources_thread(nmos_model, nmos_mutex, registration_expiration_condition, shutdown, query_ws_events_condition, gate); });

    // Configure the Node API

    web::http::experimental::listener::api_router node_api = nmos::make_node_api(self_resources, self_mutex, gate);
    web::http::experimental::listener::http_listener node_listener(web::http::experimental::listener::make_listener_uri(nmos::fields::node_port(nmos_model.settings)));
    nmos::support_api(node_listener, node_api);

    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Configuring nmos-cpp registry as node on: " << nmos::fields::host_address(nmos_model.settings) << ":" << nmos::fields::node_port(nmos_model.settings);

    // set up the node resources
    nmos::experimental::make_server_resources(self_resources, nmos_model.settings);

    // add the self resources to the registration API resources
    nmos_model.resources.insert(self_resources.begin(), self_resources.end());

    // Configure the Connection API

    web::http::experimental::listener::api_router connection_api = nmos::make_connection_api(self_resources, self_mutex, gate);
    web::http::experimental::listener::http_listener connection_listener(web::http::experimental::listener::make_listener_uri(nmos::fields::connection_port(nmos_model.settings)));
    nmos::support_api(connection_listener, connection_api);

    // Configure the Admin UI

    const utility::string_t admin_filesystem_root = U("./admin");
    web::http::experimental::listener::api_router admin_ui = nmos::experimental::make_admin_ui(admin_filesystem_root, gate);
    web::http::experimental::listener::http_listener admin_listener(web::http::experimental::listener::make_listener_uri(nmos::experimental::fields::admin_port(nmos_model.settings)));
    nmos::support_api(admin_listener, admin_ui);

    // Configure the mDNS advertisements for our APIs
    
    std::unique_ptr< mdns::service_advertiser > advertiser = mdns::make_advertiser(gate); 
    const std::vector<std::string> txt_records
    {
        "api_proto=http",
        "api_ver=v1.0,v1.1,v1.2",
        "pri=100"
    };

    advertiser->register_service("nmos-cpp_query", "_nmos-query._tcp", (uint16_t)nmos::fields::query_port(nmos_model.settings), {}, txt_records);
    advertiser->register_service("nmos-cpp_registration", "_nmos-registration._tcp", (uint16_t)nmos::fields::registration_port(nmos_model.settings), {}, txt_records);
    advertiser->register_service("nmos-cpp_node", "_nmos-node._tcp", (uint16_t)nmos::fields::node_port(nmos_model.settings), {}, txt_records);

    // Advertise our APIs

    advertiser->start();  

    try
    {
        slog::log<slog::severities::info>(gate, SLOG_FLF) << "Preparing for connections";

        // open in an order that means NMOS APIs don't expose references to others that aren't open yet

        logging_listener.open().wait();
        settings_listener.open().wait();

        node_listener.open().wait();
        connection_listener.open().wait();
        query_ws_listener.open().wait();
        query_listener.open().wait();
        registration_listener.open().wait();

        admin_listener.open().wait();

        mdns_listener.open().wait();

        slog::log<slog::severities::info>(gate, SLOG_FLF) << "Ready for connections";

        std::string command;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "Press return to quit." << std::endl;
        }
        std::cin >> std::noskipws >> command;

        slog::log<slog::severities::info>(gate, SLOG_FLF) << "Closing connections";

        // close in reverse order

        mdns_listener.close().wait();

        admin_listener.close().wait();

        registration_listener.close().wait();
        query_listener.close().wait();
        query_ws_listener.close().wait();
        connection_listener.close().wait();
        node_listener.close().wait();

        settings_listener.close().wait();
        logging_listener.close().wait();
    }
    catch (const web::http::http_exception& e)
    {
        slog::log<slog::severities::error>(gate, SLOG_FLF) << e.what() << " [" << e.error_code() << "]";
    }

    advertiser->stop();

    shutdown = true;
    registration_expiration_condition.notify_all();
    query_ws_events_condition.notify_all();
    registration_expiration.join();
    query_ws_events_sending.join();

    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Stopping nmos-cpp registry";

    return 0;
}
