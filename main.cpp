// c++ twitch bot
#define SDL_MAIN_HANDLED
#define ASIO_STANDALONE
#define ASIO_HAS_THREADS
#define CURL_STATICLIB
 
#include "websocketpp/config/asio_client.hpp"

#include "websocketpp/client.hpp"

#include <SDL.h>
#include <SDL_mixer.h>

#include <cstdio>
#include <fstream>
#include <filesystem>

namespace Files = std::filesystem;

#include "curl_wrapper.hpp"
#include "types.hpp"
#include "utilities.hpp"
#include "youtube_api.hpp"
#include "json.hpp"

CURL* curl_handle {nullptr};

using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
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

        if(irc_connector)
        {
            c->send(handle, "CAP REQ :twitch.tv/membership twitch.tv/tags twitch.tv/commands\r\n", websocketpp::frame::opcode::text);
            c->send(handle, "PASS oauth:" + AUTH_TOKEN + "\r\n", websocketpp::frame::opcode::text);
            c->send(handle, "NICK " + BROADCASTER_NAME + "\r\n", websocketpp::frame::opcode::text);
            c->send(handle, "JOIN #" + BROADCASTER_NAME + "\r\n", websocketpp::frame::opcode::text);
        }
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
        std::lock_guard<std::mutex> g {message_mutex};
        auto pay_load {msg->get_payload()};
        if(msg->get_opcode() == websocketpp::frame::opcode::text)
        {
            if(!irc_connector){
                messages.push_back("<< " + pay_load);
            }
            if(pay_load.find("JOIN") != String::npos){
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
        else
        {
            pay_load = websocketpp::utility::to_hex(pay_load);
            if(!irc_connector){
                messages.push_back("<< " + pay_load);
            }
        }
        printf("%s : %s\n", name.c_str(), pay_load.c_str());
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
    bool irc_connector;
    std::mutex message_mutex;
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
    int connect(String const & uri, const String& name, const bool irc_connector = true)
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
        metadata_ptr->irc_connector = irc_connector;

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

struct User
{
    String nick;
    String host;
    String login_name;
    String user_id;
    String badges;
};

struct Bot
{
    struct Music_Info
    {
        Youtube_Video_Info video;
        Vector<String> args;
    };

    struct Sound
    {
        String name;
        Mix_Chunk* chunk {nullptr};
    };

    struct Sound_To_Play
    {
        bool played {false};
        int channel;
        Sound* sound;
        int loops {0};
        int volume {MIX_MAX_VOLUME / 4};
    };

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
        {
            std::lock_guard<std::mutex> g {event_sub_handle->message_mutex};
            if(!event_sub_handle->messages.empty())
            {
                if(event_sub_session_id.empty())
                {
                    auto ctr {0};
                    for(const auto& s : event_sub_handle->messages)
                    {
                        event_sub_session_id = json_get_value_naive("id", s);
                        clean_line(&event_sub_session_id);
                        if(!event_sub_session_id.empty()){
                            break;
                        }
                        ctr++;
                    }
                    if(!event_sub_session_id.empty())
                    {
                        subscribe_to_event("channel.follow",
                                            2,
                                            wrap_in_quotes("condition") + ":{" + 
                                            wrap_in_quotes("broadcaster_user_id") + ":" + wrap_in_quotes(BROADCASTER_ID) + "," +
                                            wrap_in_quotes("moderator_user_id") + ":" + wrap_in_quotes(BROADCASTER_ID) + "}");

                        subscribe_to_event("channel.subscribe",
                                            1,
                                            wrap_in_quotes("condition") + ":{" + 
                                            wrap_in_quotes("broadcaster_user_id") + ":" + wrap_in_quotes(BROADCASTER_ID) + "}");

                        event_sub_handle->messages.erase(event_sub_handle->messages.begin() + ctr);
                    }
                }
                else
                {
                    auto data {event_sub_handle->messages.front()};
                    auto s {json_get_value_naive("message_type", data)};

                    event_sub_handle->messages.erase(event_sub_handle->messages.begin());
                    //twitch follow notif
                    //Event Sub : {"metadata":{"message_id":"k1SWN3SGe2BeNYxkD_cjR4maZ99PDRDd6Zx48W9t5qw=","message_type":"notification","message_timestamp":"2023-08-25T16:21:23.993248783Z","subscription_type":"channel.follow","subscription_version":"2"},"payload":{"subscription":{"id":"0929c6e1-c008-4c2f-ab6c-8d236d46afde","status":"enabled","type":"channel.follow","version":"2","condition":{"broadcaster_user_id":"445242086","moderator_user_id":"445242086"},"transport":{"method":"websocket","session_id":"AgoQU9piiL9nShG3bjSWvTLfqBIGY2VsbC1i"},"created_at":"2023-08-25T13:01:47.909083114Z","cost":0},"event":{"user_id":"726187387","user_login":"555gerson555","user_name":"555gerson555","broadcaster_user_id":"445242086","broadcaster_user_login":"coffee_lava","broadcaster_user_name":"coffee_lava","followed_at":"2023-08-25T16:21:23.993240695Z"}}}

                    if(s == "notification")
                    {
                        s = json_get_value_naive("subscription_type", data);
                        if(s == "channel.follow")
                        {
                            s = json_get_value_naive("user_name", data);
                            add_message(format_reply_2("Yow lil bro! Thanks for the follow!", s));
                        }
                        else if(s == "channel.subscribe" || s == "channel.subscription.message")
                        {
                            s = json_get_value_naive("user_name", data);
                            add_message(format_reply_2("Yow lil bro! Thanks for subbing!", s));
                        }
                    }
                }
            }
        } 
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
            printf("%s\n", pong.c_str());
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

    void check_sounds_to_play()
    {
        std::lock_guard<std::mutex> g {sound_mutex};
        if(!sounds_to_play.empty())
        {
            auto& s {sounds_to_play.front()};
            if(!s.played)
            {
                s.played = true;
                s.channel = Mix_PlayChannel(-1, s.sound->chunk, s.loops);
                Mix_Volume(s.channel, s.volume);
            }
            else
            {
                if(!Mix_Playing(s.channel)){
                    sounds_to_play.erase(sounds_to_play.begin());
                }
            }
        }
    }

    Sound* get_sound(const String s)
    {
        for(auto& sound : sounds)
        {
            if(sound.name == s){
                return &sound;
            }
        }
        return nullptr;
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

    void subscribe_to_event(const String& s, const int v, const String& c)
    {
        std::lock_guard<std::mutex> guard {curl_mutex};
        curl_easy_reset(curl_handle);

        String url {"https://api.twitch.tv/helix/eventsub/subscriptions"}; 

        auto list {set_curl_headers(("Authorization: Bearer " + AUTH_TOKEN).c_str(),
                                    ("Client-Id: " + CLIENT_ID).c_str(),
                                    "Content-Type: application/json")};

        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, list);

        String post_fields;
        post_fields = "{" + wrap_in_quotes("type") + ":" + wrap_in_quotes(s) + "," +
                            wrap_in_quotes("version") + ":" + wrap_in_quotes(std::to_string(v)) + "," +
                            c + "," + 
                            wrap_in_quotes("transport") + ":" + "{" + wrap_in_quotes("method") + ":" + wrap_in_quotes("websocket") + "," +
                                                                      wrap_in_quotes("session_id") + ":" + wrap_in_quotes(event_sub_session_id) +"}}";

        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_fields.c_str());

        auto response {curl_call(url, curl_handle, list)};
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
        for(auto& s : sounds)
        {
            if(s.chunk){
                Mix_FreeChunk(s.chunk);
            }
        }
    }

    bool experimental {false};

    int connection_id;
    int event_sub_connection_id;
    Connection_Metadata::Pointer handle;
    Connection_Metadata::Pointer event_sub_handle;
    Websocket_Endpoint end_point;
    Stamp last_music_stamp;
    std::thread music_thread;

    Vector<Command> commands;
    Vector<String> messages_to_send;

    u64 batchest_count {0};
    String today;
    String event_sub_session_id;

    Vector<Sound> sounds;
    Vector<Sound_To_Play> sounds_to_play;

    Music_Info last_song;
    Vector<Music_Info> music_queue;
    Vector<Pair<String, int>> banned_words;
    Vector<User> current_users;

    String data_file_name {"bot.txt"};

    std::mutex sound_mutex;
    std::mutex music_mutex;
    std::mutex send_mutex;
    std::mutex curl_mutex;
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

void tts_callback(Bot* b, const Vector<String>& args)
{
    std::lock_guard<std::mutex> g {b->sound_mutex};
    String n {};
    for(int i = 1; i < args.size(); i++)
    {
        const auto& s {args[i]};
        if(s[0] == '-' || s[0] == '+')
        {
            Bot::Sound_To_Play play;
            auto pipe {s.find('|')};
            auto pipe2 {s.find('|', pipe + 1)};
            n = s.substr(1, pipe - 1);
            auto sound {b->get_sound(n)};
            if(sound)
            {
                play.sound = sound;
                if(pipe != String::npos)
                {
                    float f;
                    if(pipe2 == String::npos){
                        f = string_to_float(s.substr(pipe + 1, String::npos));
                    }
                    else{
                        f = string_to_float(s.substr(pipe + 1, pipe2 - pipe));
                    }
                    play.volume = (float)play.volume * f;
                }
                if(pipe2 != String::npos)
                {
                    play.loops = string_to_int(s.substr(pipe2 + 1, String::npos));
                    if(play.loops > 5){
                        play.loops = 5;
                    }
                    if(play.loops < 0){
                        play.loops = 0;
                    }
                }
                if(s[0] == '-'){
                    b->sounds_to_play.push_back(play);
                }
                else
                {
                    play.loops = 0;
                    play.channel = Mix_PlayChannel(-1, play.sound->chunk, play.loops);
                    Mix_Volume(play.channel, play.volume);
                }
            }
        }
    }
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

void skip_sound_callback(Bot* b, const Vector<String>& args)
{
    std::lock_guard<std::mutex> g {b->sound_mutex};
    if(!b->sounds_to_play.empty())
    {
        auto& s {b->sounds_to_play.front()};
        if(s.played)
        {
            Mix_HaltChannel(s.channel);
            b->sounds_to_play.erase(b->sounds_to_play.begin());
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

    SDL_Init(SDL_INIT_AUDIO);
    Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG);
    Mix_OpenAudio(44000, MIX_DEFAULT_FORMAT, 2, 4096);
    Mix_AllocateChannels(1000000);

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
        curl_easy_reset(curl_handle);
        auto list {set_curl_headers(("Authorization: Bearer " + AUTH_TOKEN).c_str(),
                                    ("Client-Id: " + CLIENT_ID).c_str())};

        String s {curl_call(("https://api.twitch.tv/helix/users?login=" + BROADCASTER_NAME).c_str(), curl_handle, list)};
        BROADCASTER_ID = json_get_value_naive("id", s);
        curl_slist_free_all(list);
    }


    {
        std::ifstream file {"banned_words.txt"};
        String line;
        Pair<String, int> p;
        while(file>>p.first>>p.second){
            bot.banned_words.push_back(p);
        }
    }
    
    {
        const String directory {"sounds/"};
        bot.sounds.reserve(100);
        String file_name;
        for(const auto& e : Files::directory_iterator(directory))
        {
            bot.sounds.push_back({});
            auto& s {bot.sounds.back()};
            s.name = e.path().filename().stem().string();
            file_name = e.path().filename().string(); 
            s.chunk = Mix_LoadWAV((directory + file_name).c_str());
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
    bot.add_command("tts", tts_callback); 
    bot.add_command("sr", music_callback); 
    bot.add_command("skip", skip_song_callback); 
    bot.add_command("sc", music_count_callback); 
    bot.add_command("ss", skip_sound_callback, {broadcaster_badge, vip_badge, moderator_badge});
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

    bot.connection_id = bot.end_point.connect("wss://irc-ws.chat.twitch.tv:443", "Twitch API");
    bot.event_sub_connection_id = bot.end_point.connect("wss://eventsub.wss.twitch.tv/ws", "Event Sub", false);

    printf("%i\n", bot.connection_id);
    printf("%i\n", bot.event_sub_connection_id);
    if(bot.connection_id == -1 || bot.event_sub_connection_id == -1){
        printf("connection failed");
    }
    else
    {
        bot.handle = bot.end_point.connection_list[bot.connection_id];

        bot.event_sub_handle = bot.end_point.connection_list[bot.event_sub_connection_id];

        auto running {true};
        std::atomic<bool> said_welcome_message {false};

        const auto sleep_time {100};

        //auto spawn_daemon_thread {[&]<typename U>(U u)
        //{
        //    auto t {std::thread([&](U f, Bot* b){
        //        f();
        //        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        //    })};
        //    t.detach();
        //}};
        //spawn_daemon_thread();
        
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

        auto sound_thread {std::thread([&](Bot* b)
        {
            while(true)
            {
                b->check_sounds_to_play();
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
            }
        }, &bot)};
        sound_thread.detach();


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
    SDL_Quit();
}

int main(int args, const char** argc)
{

    // TODO
    // follow notif
    // sub notif
    // tts and tiktok api
    // cheer notif
    // point system
    // social credit system
    // raid notif

    start_bot(args, argc);
    
    return 0;
}
