// c++ twitch bot
#define ASIO_STANDALONE
#define ASIO_HAS_THREADS
#define CURL_STATICLIB
 
#include "websocketpp/config/asio_no_tls_client.hpp"

#include "websocketpp/client.hpp"

#include <cstdio>
#include <fstream>

#include "curl_wrapper.hpp"
#include "types.hpp"
#include "utilities.hpp"
#include "youtube_api.hpp"
#include "json.hpp"

CURL* curl_handle {nullptr};

using Client = websocketpp::client<websocketpp::config::asio_client>;
using Connection_Handle = websocketpp::connection_hdl;
using Connection_Pointer = Client::connection_ptr;

const String broadcaster_badge {"broadcaster"};
const String moderator_badge   {"moderator"};
const String vip_badge         {"vip"};
const String founder_badge     {"founder"};
const String subscriber_badge  {"subscriber"};

String CLIENT_ID        {};
String BROADCASTER_ID   {};
String AUTH_TOKEN       {};
String YOUTUBE_API_KEY  {};
String BROADCASTER_NAME {};

struct Parsed_Message
{
    String host;
    String nick;
    String message;
    String user_id;
    String badges;
};

String format_reply(const String& sender, const String& message)
{
    return "PRIVMSG #" + BROADCASTER_NAME + " :@" + sender + " " + message + "\r\n";
}

String format_reply_2(const String& message, const String& sender)
{
    return "PRIVMSG #" + BROADCASTER_NAME + " :" + message + " " + "@" + sender + "\r\n";
}

String format_send(const String& message)
{
    return "PRIVMSG #" + BROADCASTER_NAME + " :" + message + "\r\n";
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

    auto get_badges {[&]()
    {
        const auto badge_badge {"badges="};
        auto b_pos {msg.find(badge_badge)}; 
        const auto bl {strlen(badge_badge)};
        Vector<String> badges {moderator_badge, broadcaster_badge, vip_badge, founder_badge, subscriber_badge};
        String result;
        if(b_pos != String::npos)
        {
            const auto semi_colon {msg.find(';', b_pos + bl + 1)};
            auto start {b_pos + bl};
            auto badge {msg.find("/", start)};
            while(badge != String::npos)
            {
                result += msg.substr(start, badge - start);
                result += " ";
                start = badge + 2;
                if(start >= semi_colon){
                    break;
                }
                badge = msg.find("/", start);
            }
        }
        return result;
    }};

    auto get_user_id {[&]()
    {
        const auto user_id {"user-id="};
        auto pos {msg.find(user_id)};
        pos += 8;
        return msg.substr(pos, msg.find(';', pos) - pos);
    }};

    Parsed_Message result;

    result.host = get_host();
    result.nick = get_nick();
    result.message = get_message();
    result.badges = get_badges();
    result.user_id = get_user_id();

    clean_line(&result.host);
    clean_line(&result.nick);
    clean_line(&result.message);
    clean_line(&result.badges);
    clean_line(&result.user_id);

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
        c->send(handle, "PASS oauth:" + AUTH_TOKEN + "\r\n", websocketpp::frame::opcode::text);
        c->send(handle, "NICK " + BROADCASTER_NAME + "\r\n", websocketpp::frame::opcode::text);
        c->send(handle, "JOIN #" + BROADCASTER_NAME + "\r\n", websocketpp::frame::opcode::text);
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
            else
            {
                auto tokens {tokenize(pay_load)};
                if(!tokens.empty())
                {
                    if(tokens[0] == "PING"){
                        ping_messages.push_back(pay_load);
                    }
                }
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
        Vector<String> badges;
        bool no_badges;
    };

    void add_command(const String& name, Callback c, const Vector<String>& badges = {}, const bool no_badges = false)
    {
        if(experimental){
            commands.push_back({"_" + name, c, badges, no_badges});
        }
        else{
            commands.push_back({name, c, badges, no_badges});
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
            auto has_command {false};
            if(tokens[0][0] == '!')
            {
                auto c {find_command(tokens[0].substr(1, String::npos))};
                if(c)
                {
                    has_command = true;
                    bool badge_is_good {};
                    if(c->no_badges){
                        badge_is_good = sender.badges.empty(); 
                    }
                    else
                    {
                        badge_is_good = c->badges.empty();
                        for(const auto& s : c->badges)
                        {
                            if(sender.badges.find(s) != String::npos)
                            {
                                badge_is_good = true;
                                break;
                            }
                        }
                    }
                    if(badge_is_good)
                    {
                        tokens.erase(tokens.begin());
                        tokens.insert(tokens.begin(), sender.nick);
                        auto t {std::thread(c->callback, this, tokens)};
                        t.detach();
                    }
                    else{
                        add_message(format_reply(sender.nick, "You are not BatChest enough!"));
                    }
                }
            }
            if(!has_command)
            {
                String str;
                for(const auto& s : tokens)
                {
                    if(s == "BatChest"){
                        batchest_count++;
                    }
                    str += s;
                    str += ' ';
                }
                string_decapitalize(&str);
                int duration {0};
                auto has_banned_word {false};
                for(const auto& p : banned_words)
                {
                    auto start {0};
                    auto pos {str.find(p.first)};
                    while(pos != String::npos)
                    {
                        start = pos + 1;
                        duration += p.second;
                        has_banned_word = true;
                        pos = str.find(p.first, start);
                    }
                }
                if(has_banned_word){
                    ban_user(sender.user_id, duration);
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

    void ban_user(const String& id, const int dur)
    {
        std::lock_guard<std::mutex> guard {curl_mutex};
        curl_easy_reset(curl_handle);

        String url {"https://api.twitch.tv/helix/moderation/bans?broadcaster_id=" + BROADCASTER_ID + "&moderator_id=" + BROADCASTER_ID}; 

        curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());

        String user_id {wrap_in_quotes("user_id") + ":" + wrap_in_quotes(id)};


        String duration {dur == -1 ? "" : wrap_in_quotes("duration") + ":" + wrap_in_quotes(std::to_string(dur))};


        auto list {set_curl_headers(("Authorization: Bearer " + AUTH_TOKEN).c_str(),
                                    ("Client-Id: " + CLIENT_ID).c_str(),
                                    "Content-Type: application/json")};

        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, list);

        String post_fields;
        if(!duration.empty()){
            post_fields  = "{\"data\": {" + user_id + "," + duration + "}}";
        }
        else{
            post_fields  = "{\"data\": {" + user_id + "}}";
        }
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_fields.c_str());

        curl_easy_perform(curl_handle);
        curl_slist_free_all(list);
    }

    void serialize_in()
    {
        String line;
        String tag;
        String value;
        std::ifstream file {data_file_name};
        while(std::getline(file, line))
        {
            clean_line(&line);
            if(!line.empty())
            {
                extract_tag_and_value_from_line(line, &tag, &value);
                if(tag == "batchest_count"){
                    std::stringstream ss {value};
                    ss>>batchest_count;
                }
            }
        }
    }

    void serialize_out()
    {
        std::ofstream file {data_file_name};
        file<<"batchest_count : "<<batchest_count;
    }

    ~Bot()
    {
        serialize_out();
    }

    bool experimental {false};

    int connection_id;
    std::mutex music_mutex;
    std::mutex send_mutex;
    std::mutex curl_mutex;
    Connection_Metadata::Pointer handle;
    Websocket_Endpoint end_point;
    Stamp last_music_stamp;
    std::thread music_thread;

    Vector<Command> commands;
    Vector<String> messages_to_send;

    u64 batchest_count {0};
    String today;

    struct Music_Info
    {
        Youtube_Video_Info video;
        Vector<String> args;
    };

    Music_Info last_song;
    Vector<Music_Info> music_queue;
    Vector<Pair<String, int>> banned_words;

    String data_file_name {"bot.txt"};
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

    std::lock_guard<std::mutex> guard {b->curl_mutex};
    auto equals {args[1].find('=')};
    if(equals == String::npos){
        b->add_message(format_reply(args[0], "AwkwardMonkey invalid video"));
        return;
    }

    auto video       {args[1].substr(equals + 1, String::npos)}; 
    String video_arg {"&id=" + video};
    String key_arg   {"&key=" + YOUTUBE_API_KEY};
    String api       {"https://www.googleapis.com/youtube/v3/videos?"};
    String part      {"part=snippet,contentDetails,statistics"};

    curl_easy_reset(curl_handle);

    auto curl_result {curl_call(api + part + video_arg + key_arg, curl_handle)};

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

void batchest_callback(Bot* b, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "BatChest Count : " + std::to_string(b->batchest_count)));
}

void today_callback(Bot* b, const Vector<String>& args)
{
    if(!b->today.empty()){
        b->add_message(format_reply(args[0], "Today : " + b->today));
    }
}

void set_today_callback(Bot* b, const Vector<String>& args)
{
    b->today = "";
    for(int i = 1; i < args.size(); i++){
        b->today += args[i] + " ";
    }
}

void set_title_callback(Bot* b, const Vector<String>& args)
{
    std::lock_guard<std::mutex> guard {b->curl_mutex};
    curl_easy_reset(curl_handle);

    String url {"https://api.twitch.tv/helix/channels?broadcaster_id=" + BROADCASTER_ID}; 

    curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());

    auto list {set_curl_headers(("Authorization: Bearer " + AUTH_TOKEN).c_str(),
                                ("Client-Id: " + CLIENT_ID).c_str(),
                                 "Content-Type: application/json")};

    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, list);

    String post_fields;
    String title;

    for(int i = 1; i < args.size(); i++){
        title += args[i] + ' ';
    }
    post_fields  = "{" + wrap_in_quotes("title") + ":" + wrap_in_quotes(title) + "}";
    curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_fields.c_str());
    curl_easy_perform(curl_handle);
    curl_slist_free_all(list);
    b->add_message(format_send("Stream Title : " + title));
}

void founder_callback(Bot* b, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "You're a founder... founder of deez nuts! GOTTEM"));
}

void mod_callback(Bot* b, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "You're a moderator, protector of deez nuts GOTTEM"));
}

void pleb_callback(Bot* b, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "You're a pleb, cultivator of deez nuts GOTTEM"));
}

void sub_callback(Bot* b, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "You're a subscriber, nourisher of deez nuts GOTTEM"));
}

void start_bot(int args, const char** argc)
{
    if(args < 2)
    {
        printf("no broadcaster name found");
        return;
    }

    BROADCASTER_NAME = argc[1];

    curl_handle = curl_easy_init();

    Bot bot;
    {
        std::ifstream file {"token.nut"};
        file>>CLIENT_ID;
        file>>AUTH_TOKEN;
        file>>YOUTUBE_API_KEY;
    }

    {
        std::ifstream file {"banned_words.txt"};
        String line;
        Pair<String, int> p;
        while(file>>p.first>>p.second){
            bot.banned_words.push_back(p);
        }
    }
    
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
    bot.add_command("batchest", batchest_callback); 
    bot.add_command("today", today_callback); 
    bot.add_command("settoday", set_today_callback, {moderator_badge, broadcaster_badge}); 
    bot.add_command("settitle", set_title_callback, {broadcaster_badge, moderator_badge}); 
    bot.add_command("founder", founder_callback, {founder_badge}); 
    bot.add_command("mod", mod_callback, {moderator_badge}); 
    bot.add_command("sub", sub_callback, {subscriber_badge}); 
    bot.add_command("pleb", pleb_callback, {}, true); 

    bot.connection_id = bot.end_point.connect("ws://irc-ws.chat.twitch.tv:80");

    {
        curl_easy_reset(curl_handle);
        auto list {set_curl_headers(("Authorization: Bearer " + AUTH_TOKEN).c_str(),
                                    ("Client-Id: " + CLIENT_ID).c_str())};

        String s {curl_call(("https://api.twitch.tv/helix/users?login=" + BROADCASTER_NAME).c_str(), curl_handle, list)};
        BROADCASTER_ID = json_get_value_naive("id", s);
        curl_slist_free_all(list);
    }

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
                    bot.add_message("PRIVMSG #" + BROADCASTER_NAME + " :coffee481Happy BatChest coffee481Happy\r\n");
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
    // settitle
    // tts and tiktok api
    // follow notif
    // sub notif
    // point system
    // social credit system
    // raid notif

    start_bot(args, argc);
    
    return 0;
}
