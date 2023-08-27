#pragma once

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

    using Pointer = websocketpp::lib::shared_ptr<Connection_Metadata>;
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
        why_failed = cptr->get_ec().message();
        printf("%s\n", why_failed.c_str());
    }

    void on_close(Client * c, websocketpp::connection_hdl hdl) {
        status = "Closed";
        Client::connection_ptr con = c->get_con_from_hdl(hdl);
        std::stringstream s;
        s << "close code: " << con->get_remote_close_code() << " (" 
          << websocketpp::close::status::get_string(con->get_remote_close_code()) 
          << "), close reason: " << con->get_remote_close_reason();
        why_failed = s.str();
        printf("%s\n", why_failed.c_str());
    }

    void on_message(Connection_Handle, Client::message_ptr msg)
    {
        on_message_handler(this, msg);
    }
    void record_sent_message(String message) {
        //messages.push_back(">> " + message);
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
    String why_failed;

    std::atomic<bool> joined {false};
    bool opened {false};
};

struct Websocket_Endpoint
{
    Websocket_Endpoint () : next_id(0)
    {
        end_point.clear_access_channels(websocketpp::log::alevel::all);
        end_point.clear_error_channels(websocketpp::log::elevel::all);

        end_point.init_asio();

        end_point.set_tls_init_handler(websocketpp::lib::bind(on_tls_init));

        end_point.start_perpetual();

        thread = websocketpp::lib::make_shared<websocketpp::lib::thread>(&Client::run, &end_point);
    }

    using context_ptr = std::shared_ptr<asio::ssl::context>;

	static context_ptr on_tls_init()
	{
		context_ptr ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::sslv23);

		try {
			ctx->set_options(
				asio::ssl::context::default_workarounds
				| asio::ssl::context::no_sslv2
				| asio::ssl::context::no_sslv3
				| asio::ssl::context::single_dh_use
			);

		} catch (std::exception &e)
		{
			std::cout << "Error in context pointer: " << e.what() << std::endl;
		}
		return ctx;
	}
    int connect(String const & uri, const String& name, Connection_Metadata::On_Message_Handler omh)
    {
        websocketpp::lib::error_code ec;

        auto con {end_point.get_connection(uri, ec)};

        if(ec)
        {
            std::cout << "> Connect initialization error: " << ec.message() << std::endl;
            return -1;
        }

        auto new_id {next_id++};
        auto metadata_ptr {websocketpp::lib::make_shared<Connection_Metadata>(new_id, con->get_handle(), uri)};
        connection_list[new_id] = metadata_ptr;
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

    void send(int id, String message)
    {
        websocketpp::lib::error_code ec;
        
        auto metadata_it {connection_list.find(id)};
        if (metadata_it == connection_list.end()) {
            std::cout << "> No connection found with id " << id << std::endl;
            return;
        }
        
        end_point.send(metadata_it->second->handle, message, websocketpp::frame::opcode::text, ec);
        if (ec) {
            std::cout << "> Error sending message: " << ec.message() << std::endl;
            return;
        }
        
        metadata_it->second->record_sent_message(message);
    }

    Connection_Metadata::Pointer get_metadata(int id) const
    {
        auto metadata_it {connection_list.find(id)};
        if(metadata_it == connection_list.end()){
            return Connection_Metadata::Pointer();
        }
        else{
            return metadata_it->second;
        }
    }

    using Connection_List = std::map<int, Connection_Metadata::Pointer>;
    int next_id;
    Connection_List connection_list;
    Client end_point;
    websocketpp::lib::shared_ptr<websocketpp::lib::thread> thread;
};
