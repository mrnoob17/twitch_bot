// c++ twitch bot
#define ASIO_STANDALONE
#define ASIO_HAS_THREADS
 
#include "websocketpp/config/asio_no_tls_client.hpp"

#include "websocketpp/client.hpp"

#include <cstdio>
#include <fstream>
#include <chrono>

using Clock = std::chrono::high_resolution_clock;
using Stamp = decltype(Clock::now());
using Duration = std::chrono::duration<float>;

struct Timer
{
    Stamp begin;
    float wait {0};
    bool started {false};
    void start(const float dur)
    {
        wait = dur;
        started = true;
        begin = Clock::now();
    }

    void stop()
    {
        started = false;
    }

    bool is_time()
    {
        if(!started){
            return false;
        }
        auto end {Clock::now()};
        Duration elapsed {end - begin};
        if(elapsed.count() >= wait){
            return true;
        }
        return false;
    }
};

using Client = websocketpp::client<websocketpp::config::asio_client>;
using Connection_Handle = websocketpp::connection_hdl;
using Connection_Pointer = Client::connection_ptr;

using String = std::string;

template<typename T>
using Vector = std::vector<T>;

String auth_token       {""};
String broadcaster_name {""};

struct Parsed_Message
{
    String host;
    String nick;
    String message;
};

void clean_line(String* str)
{
    if(str->empty()){
        return;
    }
    auto cleanable {[](const char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n'; 
    }};

    while(cleanable(str->front()))
    {
        str->erase(0, 1); 
        if(str->empty()){
            return;
        }
    }

    while(cleanable(str->back()))
    {
        str->pop_back();
        if(str->empty()){
            return;
        }
    }
}

Vector<String> tokenize(const String& str)
{
    if(str.empty()){
        return {};
    }
    Vector<String> result;
    int off {0};
    auto find {str.find(" ")};
    if(find == str.npos){
        result.push_back(str);
    }
    else
    {
        while(find != str.npos)
        {
            result.push_back(str.substr(off, find - off));
            off = find + 1;
            find = str.find(" ", off);
        }
        if(off < str.length()){
            result.push_back(str.substr(off, str.npos));
        }
    }
    for(auto& s : result){
        clean_line(&s);
    }
    return result;

}

String format_reply(const String& sender, const String& message)
{
    return "PRIVMSG #" + broadcaster_name + " :@" + sender + " " + message + "\r\n";
}


String format_reply_2(const String& message, const String& sender)
{
    return "PRIVMSG #" + broadcaster_name + " :" + message + " " + "@" + sender + "\r\n";
}

Parsed_Message parse_message(const String& msg)
{
    if(msg.find("PRIVMSG") == msg.npos){
        return {};
    }

    auto get_host {[&]()
    {
        auto colon {msg.find(" :") + 2};
        return msg.substr(colon, msg.find(" ", colon + 1) - colon);
    }};

    auto get_nick {[&]()
    {
        auto colon {msg.find(" :") + 2};
        return msg.substr(colon, msg.find("!", colon + 1) - colon);
    }};

    auto get_message {[&]()
    {
        auto fc {msg.find(" :")};
        auto colon {msg.find(" :", fc + 2)};
        return msg.substr(colon + 2, msg.npos);
    }};

    Parsed_Message result;

    result.host = get_host();
    result.nick = get_nick();
    result.message = get_message();

    clean_line(&result.host);
    clean_line(&result.nick);
    clean_line(&result.message);

    return result;
}

struct Connection_Metadata
{
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

        c->send(handle, "CAP REQ :twitch.tv/membership twitch.tv/tags twitch.tv/commands\r\n", websocketpp::frame::opcode::text);
        c->send(handle, "PASS oauth:" + auth_token + "\r\n", websocketpp::frame::opcode::text);
        c->send(handle, "NICK " + broadcaster_name + "\r\n", websocketpp::frame::opcode::text);
        c->send(handle, "JOIN #" + broadcaster_name + "\r\n", websocketpp::frame::opcode::text);
    }
    
    void on_fail(Client* c, Connection_Handle handle)
    {
        status = "Failed";
        auto cptr {c->get_con_from_hdl(handle)}; 
        server = cptr->get_response_header("Server");
        why_failed = cptr->get_ec().message();

    }

    void on_close(Client * c, websocketpp::connection_hdl hdl) {
        status = "Closed";
        Client::connection_ptr con = c->get_con_from_hdl(hdl);
        std::stringstream s;
        s << "close code: " << con->get_remote_close_code() << " (" 
          << websocketpp::close::status::get_string(con->get_remote_close_code()) 
          << "), close reason: " << con->get_remote_close_reason();
        why_failed = s.str();
    }

    void on_message(Connection_Handle, Client::message_ptr msg)
    {
        if(msg->get_opcode() == websocketpp::frame::opcode::text)
        {
            messages.push_back("<< " + msg->get_payload());
            if(messages.back().find("JOIN") != String::npos){
                joined = true;
            }

            auto parsed_message {parse_message(messages.back())};
            if(!parsed_message.message.empty()){
                priv_messages.push_back(parsed_message);
            }
        }
        else{
            messages.push_back("<< " + websocketpp::utility::to_hex(msg->get_payload()));
        }
        printf("%s", messages.back().c_str());
    }
    void record_sent_message(String message) {
        messages.push_back(">> " + message);
    }

    int id;
    Connection_Handle handle;
    String uri;
    String status;
    String server;
    Vector<String> messages;
    Vector<Parsed_Message> priv_messages;
    String why_failed;
    std::atomic<bool> joined {false};
};

struct Websocket_Endpoint
{
    Websocket_Endpoint () : next_id(0)
    {
        end_point.clear_access_channels(websocketpp::log::alevel::all);
        end_point.clear_error_channels(websocketpp::log::elevel::all);

        end_point.init_asio();
        end_point.start_perpetual();

        thread = websocketpp::lib::make_shared<websocketpp::lib::thread>(&Client::run, &end_point);
    }
    int connect(String const & uri)
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



struct Bot;

void check_music_queue(Bot*);

struct Bot
{

    Bot()
    {
        music_thread = std::thread(check_music_queue, this);
        music_thread.detach();
    }

    using Callback = void(*)(Bot*, const Vector<String>& args);

    struct Command
    {
        String name;
        Callback callback;
    };


    Vector<Command> commands;

    void add_command(const String& name, Callback c){
        commands.push_back({name, c});
    }

    Command* find_command(const String& name)
    {
        for(auto& c : commands)
        {
            if(c.name == name){
                return &c;
            }
        }
        return nullptr;
    }

    int connection_id;
    std::mutex music_thread_mutex;
    Websocket_Endpoint end_point;
    std::thread music_thread;
    Vector<Vector<String>> music_queue;
};

void check_music_queue(Bot* b)
{
    std::lock_guard<std::mutex> g {b->music_thread_mutex};
    while(!b->music_queue.empty())
    {
        auto c {b->find_command("sr")};
        c->callback(b, b->music_queue.front());
        b->music_queue.erase(b->music_queue.begin());
    }
}

void commands_callback(Bot* b, const Vector<String>& args)
{
    String list;
    for(auto& c : b->commands){
        list += c.name + " "; 
    }
   b->end_point.send(b->connection_id, format_reply(args[0], list));
}

void stack_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "stack deez nuts in your mouth GOTTEM"));
}

void drop_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "drop deez nuts in your mouth GOTTEM"));
}

void discord_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "join my discord https://discord.gg/9xVKDekJtg"));
}

void game_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "working on a roguelike called Morphus! https://store.steampob->end_pointred.com/app/2371310/Morphus/"));
}

void editor_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "neovim baseg"));
}

void font_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "https://github.com/nathco/Office-Code-Pro"));
}

void keyboard_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "keychron k8 + kailh white switches"));
}

void vimconfig_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "https://github.com/mrnoob17/my-nvim-init/blob/main/init.lua"));
}

void os_callback(Bot* b, const Vector<String>& args)
{
   b->end_point.send(b->connection_id, format_reply(args[0], "not linux baseg"));
}

void music_callback(Bot* b, const Vector<String>& args)
{
    const auto song_duration {"dur.txt"};

    String dc {"yt-dlp --print duration " + args[1] + " > " + song_duration};

    system(dc.c_str());

    auto duration {[&]()
    {
        std::ifstream file {song_duration};
        int dur;
        file>>dur;
        return dur;
    }};

    const auto d {duration()};

    if(d > 600)
    {
        b->end_point.send(b->connection_id, format_reply(args[0], "music way too long"));
        return;
    }

    {
        std::lock_guard<std::mutex> g {b->music_thread_mutex};
        if(!b->music_queue.empty())
        {
            b->music_queue.push_back(args);
            return;
        }
    }

    system("del *.mp3");

    String c {"yt-dlp -x --audio-format mp3 " + args[1]};

    auto result {system(c.c_str())};

    if(result == 1){
        b->end_point.send(b->connection_id, format_reply(args[0], "bad video link"));
    }
    else
    {
        b->end_point.send(b->connection_id, format_reply_2("current music requested by", args[0]));
        system("mpv *.mp3");
    }
}

void start_bot(int args, const char** argc)
{
    if(args < 2)
    {
        printf("you need to pass in the broadcaster name!");
        return;
    }

    broadcaster_name = argc[1];

    {
        std::ifstream file {"token.nut"};
        file>>auth_token;
    }

    Bot bot;

    bot.add_command("commands", commands_callback); 
    bot.add_command("stack", stack_callback); 
    bot.add_command("drop", drop_callback); 
    bot.add_command("discord", discord_callback); 
    bot.add_command("game", game_callback); 
    bot.add_command("editor", editor_callback); 
    bot.add_command("font", font_callback); 
    bot.add_command("keyboard", keyboard_callback); 
    bot.add_command("vimconfig", vimconfig_callback); 
    bot.add_command("os", os_callback); 
    bot.add_command("sr", music_callback); 

    bot.connection_id = bot.end_point.connect("ws://irc-ws.chat.twitch.tv:80");

    printf("%i\n", bot.connection_id);
    if(bot.connection_id == -1){
        printf("connection failed");
    }
    else
    {
        auto handle {bot.end_point.connection_list[bot.connection_id]};
        auto running {true};
        std::atomic<bool> said_welcome_message {false};
        while(running)
        {
            if(!said_welcome_message)
            {
                if(handle->joined)
                {
                    bot.end_point.send(bot.connection_id, "PRIVMSG #" + broadcaster_name + " :BatChest bot has started\r\n");
                    said_welcome_message = true;
                }
            }
            if(!handle->priv_messages.empty())
            {
                const auto& sender {handle->priv_messages.front()};
                auto tokens {tokenize(sender.message)};
                if(tokens[0][0] == '!')
                {
                    auto c {bot.find_command(tokens[0].substr(1, String::npos))};
                    if(c)
                    {
                        tokens.erase(tokens.begin());
                        tokens.insert(tokens.begin(), sender.nick);
                        c->callback(&bot, tokens);
                    }
                }
                handle->priv_messages.erase(handle->priv_messages.begin());
            }
        }
    }
}

int main(int args, const char** argc)
{

    // TODO
    // tts and tiktok api
    // need to have music folder for the music requests
    // skip music
    // music_queue
    // music request
    // follow notif
    // sub notif
    // point system
    // collect data and sell it
    // raid notif

    start_bot(args, argc);
    
    return 0;
}
