#pragma once

#define _WEBSOCKETPP_CPP11_MEMORY_
#define ASIO_STANDALONE
#define ASIO_HAS_THREADS

#include "websocketpp/config/asio_client.hpp"

#include "websocketpp/client.hpp"

#include "types.hpp"
#include "utilities.hpp"

using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
using Connection_Handle = websocketpp::connection_hdl;
using Connection_Pointer = Client::connection_ptr;

struct Parsed_Message;

struct Connection_Metadata
{

    using On_Message_Handler = void(*)(Connection_Metadata*, Client::message_ptr); 

    Connection_Metadata(const int _id, Connection_Handle _handle, const String& _uri) : id(_id), handle(_handle), uri(_uri)
    {
        status = "Connecting";
        server = "N/A";
    }

    void on_open(Client* c, Connection_Handle handle)
    {
        status = "Open";
        auto cptr {c->get_con_from_hdl(handle)}; 
        server = cptr->get_response_header("Server");

        std::unique_lock l{opened_mutex};
        opened = true;
        opened_condition_variable.notify_one();
    }
    
    void on_fail(Client* c, Connection_Handle handle)
    {
        status = "Failed";
        auto cptr {c->get_con_from_hdl(handle)}; 
        server = cptr->get_response_header("Server");
        reason = cptr->get_ec().message();
        printf("%s\n", reason.c_str());
    }

    void on_close(Client* c, websocketpp::connection_hdl hdl) {

        status = "Closed";
        auto con {c->get_con_from_hdl(hdl)};
        auto code {con->get_remote_close_code()};

        std::stringstream s;
        s << "close code: " << code << " (" 
          << websocketpp::close::status::get_string(code) 
          << "), close reason: " << con->get_remote_close_reason();

        printf("%s\n", reason.c_str());
    }

    void on_message(Connection_Handle, Client::message_ptr msg){
        on_message_handler(this, msg);
    }

    int id;

    Connection_Handle handle;
    String uri;
    String status;
    String server;
    String name;

    On_Message_Handler on_message_handler;
    std::mutex message_mutex;
    std::mutex opened_mutex;
    std::condition_variable opened_condition_variable;

    Vector<String> messages;
    String reason;

    std::atomic<bool> joined {false};
    bool opened {false};
};

struct Websocket_Endpoint
{
    using Asio_Context = std::shared_ptr<asio::ssl::context>;
    static Asio_Context on_tls_init()
    {
        Asio_Context ctx {std::make_shared<asio::ssl::context>(asio::ssl::context::sslv23)};
    
        try
        {
        	ctx->set_options(
        		asio::ssl::context::default_workarounds
        		| asio::ssl::context::no_sslv2
        		| asio::ssl::context::no_sslv3
        		| asio::ssl::context::single_dh_use
        	);
        
        }
        catch (std::exception &e){
        	printf("Error in context pointer: %s\n", e.what());
        }
        return ctx;
    }

    Websocket_Endpoint () : next_id(0)
    {
        end_point.clear_access_channels(websocketpp::log::alevel::all);
        end_point.clear_error_channels(websocketpp::log::elevel::all);

        end_point.init_asio();

        end_point.set_tls_init_handler(websocketpp::lib::bind(on_tls_init));

        end_point.start_perpetual();

        thread = websocketpp::lib::make_shared<websocketpp::lib::thread>(&Client::run, &end_point);
    }

    int connect(const String& uri, const String& name, Connection_Metadata::On_Message_Handler omh)
    {
        websocketpp::lib::error_code ec;

        auto con {end_point.get_connection(uri, ec)};

        if(ec)
        {
            printf("> Connect initialization error: %s\n", ec.message().c_str());
            return -1;
        }

        auto new_id {next_id++};
        auto metadata_ptr {websocketpp::lib::make_shared<Connection_Metadata>(new_id, con->get_handle(), uri)};
        connection_list[new_id] = metadata_ptr.get();
        metadata_ptr->name = name;
        metadata_ptr->on_message_handler = omh;

        con->set_open_handler(websocketpp::lib::bind(
            &Connection_Metadata::on_open,
            metadata_ptr,
            &end_point,
            websocketpp::lib::placeholders::_1
        ));

        con->set_fail_handler(websocketpp::lib::bind(
            &Connection_Metadata::on_fail,
            metadata_ptr,
            &end_point,
            websocketpp::lib::placeholders::_1
        ));

        con->set_close_handler(websocketpp::lib::bind(
            &Connection_Metadata::on_close,
            metadata_ptr,
            &end_point,
            websocketpp::lib::placeholders::_1
        ));

        con->set_message_handler(websocketpp::lib::bind(
            &Connection_Metadata::on_message,
            metadata_ptr,
            websocketpp::lib::placeholders::_1,
            websocketpp::lib::placeholders::_2
        ));

        end_point.connect(con);

        return new_id;
    }

    void send(const int id, const String& message)
    {
        websocketpp::lib::error_code ec;
        
        auto metadata {connection_list[id]};
        if(!metadata)
        {
            printf("> No connection found with id : %i\n", id);
            return;
        }
        
        end_point.send(metadata->handle, message, websocketpp::frame::opcode::text, ec);
        if(ec)
        {
            printf("> Error sending message: %s\n", ec.message().c_str());
            return;
        }
    }

    Connection_Metadata* get_metadata(int id)
    {
        return connection_list[id];
    }

    private:
        using Connection_List = std::map<int, Connection_Metadata*>;
        int next_id;
        Connection_List connection_list;
        Client end_point;
        websocketpp::lib::shared_ptr<websocketpp::lib::thread> thread;
};
