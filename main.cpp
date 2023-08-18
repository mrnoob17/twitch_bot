// c++ twitch bot
#define ASIO_STANDALONE
#define ASIO_HAS_THREADS
#define CURL_STATICLIB
 
#include "websocketpp/config/asio_no_tls_client.hpp"

#include "websocketpp/client.hpp"

#include <cstdio>
#include <fstream>
#include <chrono>
#include <curl/curl.h>

CURL* curl_handle {nullptr};

using Client = websocketpp::client<websocketpp::config::asio_client>;
using Connection_Handle = websocketpp::connection_hdl;
using Connection_Pointer = Client::connection_ptr;

using String = std::string;

template<typename T>
using Vector = std::vector<T>;

using Clock = std::chrono::high_resolution_clock;
using Stamp = decltype(Clock::now());
using Duration = std::chrono::duration<float>;

struct CUrl_Result
{
    size_t size    {0};
    char* response {nullptr};
};

struct Youtube_Video_Info
{
    String title    {};
    String iso_8601_duration;
    int like_count  {-1};
    int view_count  {-1};
    size_t duration {0};
};

static size_t curl_callback(void *data, size_t size, size_t nmemb, void *clientp)
{
    auto real_size {size * nmemb};
    auto mem {(CUrl_Result*)clientp};
    
    auto ptr {(char*)realloc(mem->response, mem->size + real_size + 1)};
    if(ptr == NULL){
      return 0;
    }
    
    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), data, real_size);
    mem->size += real_size;
    mem->response[mem->size] = 0;
    
    return real_size;
}
 
String curl_call(const String& s)
{
    CUrl_Result chunk {0};
    CURLcode res;
    if(curl_handle)
    {
        auto deez {curl_easy_setopt(curl_handle, CURLOPT_URL, s.c_str())};

        deez = curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_callback);
     
        deez = curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
     
        res = curl_easy_perform(curl_handle);
    }
    String str {chunk.response}; 
    free(chunk.response);
    return str;
}

Youtube_Video_Info parse_youtube_api_result(const String& s)
{
    String line;
    std::stringstream ss {s};

    auto string_to_int{[](const String& s)
    {
        std::stringstream ss {s};
        int res;
        ss>>res;
        return res;
    }};

    auto get_content {[](const String& l)
    {
        auto colon {l.find(':')};
        auto first_comma {l.find('"', colon + 1)};
        auto second_comma {l.find('"', first_comma + 1)};
        return l.substr(first_comma + 1, (second_comma - first_comma) - 1);
    }};

    auto convert_iso_8601_to_seconds {[string_to_int](const String& s)
    {
        auto get_number_component {[&s, string_to_int](size_t ctr)
        {
            String value;
            
            while(isdigit(s[ctr]))
            {
                value = s[ctr] + value;
                ctr--;
            }
            return string_to_int(value);
        }};

        int day_value     {0};
        int hours_value   {0};
        int minutes_value {0};
        int seconds_value {0};

        auto key {s.find("DT")};
        if(key != s.npos){
            day_value = get_number_component(key - 1);
        }
        key = s.find('H');
        if(key != s.npos){
            hours_value = get_number_component(key - 1);
        }
        key = s.find('M');
        if(key != s.npos){
            minutes_value = get_number_component(key - 1);
        }
        key = s.find('S');
        if(key != s.npos){
            seconds_value = get_number_component(key - 1);
        }
        return (day_value * 24 * 60 * 60) + (hours_value * 60 * 60) + (minutes_value * 60) + seconds_value;
    }};

    Youtube_Video_Info result;

    auto entered_snippet         {false};
    auto entered_content_details {false};
    auto entered_statistics      {false};

    while(std::getline(ss, line))
    {
        if(line.find("snippet") != String::npos){
            entered_snippet = true;
        }
        else if(line.find("contentDetails") != String::npos)
        {
            entered_content_details = true;
            entered_snippet = false;
        }
        else if(line.find("statistics") != String::npos)
        {
            entered_content_details = false;
            entered_snippet = false;
            entered_statistics = true;
        }
        if(entered_snippet)
        {
            if(line.find("title") != String::npos){
                result.title = get_content(line);
            }
        }
        else if(entered_content_details)
        {
            if(line.find("duration") != String::npos)
            {
                auto d {get_content(line)};
                result.iso_8601_duration = d;
                result.duration = convert_iso_8601_to_seconds(d);
            }
        }
        else if(entered_statistics)
        {
            if(line.find("viewCount") != String::npos){
                result.view_count = string_to_int(get_content(line));
            }
            else if(line.find("likeCount") != String::npos)
            {
                result.like_count = string_to_int(get_content(line));
            }
        }
    }
    return result;
};


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

String auth_token       {};
String youtube_api_key  {};
String broadcaster_name {};

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
        const auto& pay_load {msg->get_payload()};
        if(msg->get_opcode() == websocketpp::frame::opcode::text)
        {
            messages.push_back("<< " + pay_load);
            if(messages.back().find("JOIN") != String::npos){
                joined = true;
            }
            auto parsed_message {parse_message(pay_load)};
            if(!parsed_message.message.empty()){
                priv_messages.push_back(parsed_message);
            }
            if(pay_load.find("PING") != String::npos){
                ping_messages.push_back(pay_load);
            }
        }
        else{
            messages.push_back("<< " + websocketpp::utility::to_hex(pay_load));
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
    Vector<String> ping_messages;
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

struct Bot
{
    using Callback = void(*)(Bot*, const Vector<String>& args);

    struct Command
    {
        String name;
        Callback callback;
    };

    void add_command(const String& name, Callback c)
    {
        if(experimental){
            commands.push_back({"_" + name, c});
        }
        else{
            commands.push_back({name, c});
        }
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

    void check_messages()
    {
        if(!handle->priv_messages.empty())
        {
            const auto& sender {handle->priv_messages.front()};
            auto tokens {tokenize(sender.message)};
            if(tokens[0][0] == '!')
            {
                auto c {find_command(tokens[0].substr(1, String::npos))};
                if(c)
                {
                    tokens.erase(tokens.begin());
                    tokens.insert(tokens.begin(), sender.nick);
                    auto t {std::thread(c->callback, this, tokens)};
                    t.detach();
                }
            }
            handle->priv_messages.erase(handle->priv_messages.begin());
        }
        if(!handle->ping_messages.empty())
        {
            auto& pong {handle->ping_messages.front()};
            pong[1] = 'O';
            add_message(pong);
            printf("%s", pong.c_str());
            handle->ping_messages.erase(handle->ping_messages.begin());
        }
    }

    void check_music_queue()
    {
        std::lock_guard<std::mutex> g {music_mutex};
        if(!music_queue.empty())
        {
            const auto can_play {!music_playing()};
            if(can_play)
            {
                const auto& music {music_queue.front()};
    
                String str {"start /min mpv " + music.args[1] + " --no-video"};
    
                auto result {system(str.c_str())};
                if(result == 0)
                {
                    add_message(format_reply_2("Song : " + music.video.title + " requested ->", music.args[0]));
                    last_song = music;
                    last_music_stamp = Clock::now();
                    music_queue.erase(music_queue.begin());
                }
                else
                {
                    music_queue.erase(music_queue.begin());
                    add_message(format_reply(music.args[0], "something went wrong..."));
                }
            }
        }
    }

    bool music_playing()
    {
        if(last_song.args.empty()){
            return false;
        }
        Duration d {Clock::now() - last_music_stamp};
        return (d.count() < last_song.video.duration);
    }

    void send_messages()
    {
        std::lock_guard<std::mutex> g {send_mutex};
        for(const auto& s : messages_to_send){
            end_point.send(connection_id, s);
        }
        messages_to_send.clear();
    }

    void add_message(const String& str)
    {
        std::lock_guard<std::mutex> g {send_mutex};
        messages_to_send.push_back(str);
    }

    bool experimental {false};

    int connection_id;
    std::mutex music_mutex;
    std::mutex send_mutex;
    Connection_Metadata::Pointer handle;
    Websocket_Endpoint end_point;
    Stamp last_music_stamp;
    std::thread music_thread;

    Vector<Command> commands;
    Vector<String> messages_to_send;

    struct Music_Info
    {
        Youtube_Video_Info video;
        Vector<String> args;
    };

    Music_Info last_song;
    Vector<Music_Info> music_queue;
};

void commands_callback(Bot* b, const Vector<String>& args)
{
    String list;
    for(auto& c : b->commands){
        list += c.name + " "; 
    }
    b->add_message(format_reply(args[0], list));
}

void stack_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "stack deez nuts in your mouth GOTTEM"));
}

void drop_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "drop deez nuts in your mouth GOTTEM"));
}

void discord_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "join my discord https://discord.gg/9xVKDekJtg"));
}

void game_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Steam : working on a roguelike called Morphus! https://store.steampowered.com/app/2371310/Morphus/"));
}

void editor_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Editor : neovim baseg"));
}

void font_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Font : https://github.com/nathco/Office-Code-Pro"));
}

void keyboard_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Keyboard : keychron k8 + kailh white switches"));
}

void engine_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Engine : c++ and SDL2"));
}

void vimconfig_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Vim Config : https://github.com/mrnoob17/my-nvim-init/blob/main/init.lua"));
}

void friends_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Friends : twitch.tv/azenris twitch.tv/tk_dev twitch.tv/tkap1 twitch.tv/athano twitch.tv/cakez77 twitch.tv/tapir2342"));
}

void os_callback(Bot* b, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "OS : not linux baseg"));
}

void music_callback(Bot* b, const Vector<String>& args)
{

    auto equals {args[1].find('=')};
    if(equals == String::npos){
        b->add_message(format_reply(args[0], "AwkwardMonkey invalid video"));
        return;
    }

    auto video       {args[1].substr(equals + 1, String::npos)}; 
    String video_arg {"&id=" + video};
    String key_arg   {"&key=" + youtube_api_key};
    String api       {"https://www.googleapis.com/youtube/v3/videos?"};
    String part      {"part=snippet,contentDetails,statistics"};

    auto curl_result {curl_call(api + part + video_arg + key_arg)};

    auto yt_video {parse_youtube_api_result(curl_result)};

    if(yt_video.duration > 600)
    {
        b->add_message(format_reply(args[0], "AwkwardMonkey video too long"));
        return;
    }
    else if(yt_video.title.empty())
    {
        b->add_message(format_reply(args[0], "AwkwardMonkey invalid video"));
        return;
    }
    else if(yt_video.like_count < 1000)
    {
        b->add_message(format_reply(args[0], "Susge video kinda sus"));
        return;
    }
    else if(yt_video.view_count < 1000)
    {
        b->add_message(format_reply(args[0], "Susge video kinda sus"));
        return;
    }

    b->add_message(format_reply(args[0], "FeelsOkayMan song added to the queue"));
    std::lock_guard<std::mutex> g {b->music_mutex};

    b->music_queue.push_back({yt_video, args});
}

void skip_song_callback(Bot* b, const Vector<String>& args)
{
    if(!b->music_playing()){
        b->add_message(format_reply(args[0], "Aware no music is playing"));
    }
    else
    {
        if(b->last_song.args[0] == args[0])
        {
            system("taskkill /f /t /IM mpv.exe");
            b->last_song = {};
        }
        else{
            b->add_message(format_reply(args[0], "Clueless can't skip other people's songs"));
        }
    }
}

void music_count_callback(Bot* b, const Vector<String>& args)
{
    std::lock_guard<std::mutex> g {b->music_mutex};
    b->add_message(format_reply(args[0], std::to_string(b->music_queue.size()) + " song(s) in the queue"));
}

void song_callback(Bot* b, const Vector<String>& args)
{
    if(b->music_playing()){
        b->add_message(format_reply(args[0], "Song : " + b->last_song.video.title));
    }
}

void bot_callback(Bot* b, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "C++ Twitch Bot : https://github.com/mrnoob17/twitch_bot"));
}

void start_bot(int args, const char** argc)
{
    if(args < 2)
    {
        printf("no broadcaster name found");
        return;
    }

    curl_handle = curl_easy_init();

    broadcaster_name = argc[1];

    {
        std::ifstream file {"token.nut"};
        file>>auth_token;
        file>>youtube_api_key;
    }

    Bot bot;
    bot.experimental = true;

    bot.add_command("commands", commands_callback); 
    bot.add_command("stack", stack_callback); 
    bot.add_command("drop", drop_callback); 
    bot.add_command("discord", discord_callback); 
    bot.add_command("game", game_callback); 
    bot.add_command("editor", editor_callback); 
    bot.add_command("engine", engine_callback); 
    bot.add_command("font", font_callback); 
    bot.add_command("keyboard", keyboard_callback); 
    bot.add_command("vimconfig", vimconfig_callback); 
    bot.add_command("friends", friends_callback); 
    bot.add_command("os", os_callback); 
    bot.add_command("sr", music_callback); 
    bot.add_command("skip", skip_song_callback); 
    bot.add_command("sc", music_count_callback); 
    bot.add_command("song", song_callback); 
    bot.add_command("bot", bot_callback); 

    bot.connection_id = bot.end_point.connect("ws://irc-ws.chat.twitch.tv:80");

    printf("%i\n", bot.connection_id);
    if(bot.connection_id == -1){
        printf("connection failed");
    }
    else
    {
        bot.handle = bot.end_point.connection_list[bot.connection_id];
        auto running {true};
        std::atomic<bool> said_welcome_message {false};

        const auto sleep_time {100};

        auto message_thread {std::thread([&](Bot* b)
        {
            while(true)
            {
                b->check_messages();
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
            }
        }, &bot)};
        message_thread.detach();

        auto music_thread {std::thread([&](Bot* b)
        {
            while(true)
            {
                b->check_music_queue();
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
            }
        }, &bot)};
        music_thread.detach();

        auto send_thread {std::thread([&](Bot* b)
        {
            while(true)
            {
                b->send_messages();
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
            }
        }, &bot)};
        send_thread.detach();

        while(running)
        {
            if(!said_welcome_message)
            {
                if(bot.handle->joined)
                {
                    bot.add_message("PRIVMSG #" + broadcaster_name + " :coffee481Happy BatChest coffee481Happy\r\n");
                    said_welcome_message = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        }
    }
    curl_easy_cleanup(curl_handle);
}

int main(int args, const char** argc)
{

    // TODO
    // use curl with youtube api
    // curl "https://www.googleapis.com/youtube/v3/videos?part=snippet&id=<VIDEO ID>&key=<API-KEY>"
    // tts and tiktok api
    // follow notif
    // sub notif
    // point system
    // collect data and sell it
    // raid notif

    start_bot(args, argc);
    
    return 0;
}
