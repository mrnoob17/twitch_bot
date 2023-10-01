// c++ twitch bot
#define SDL_MAIN_HANDLED
#define CURL_STATICLIB
 
#include <SDL.h>
#include <SDL_mixer.h>

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <random>

namespace Files = std::filesystem;

#include "websocket.hpp"
#include "curl_wrapper.hpp"
#include "types.hpp"
#include "utilities.hpp"
#include "youtube_api.hpp"
#include "json.hpp"

CURL* curl_handle {nullptr};

std::knuth_b GENERATOR;

const String broadcaster_badge {"broadcaster"};
const String moderator_badge   {"moderator"};
const String vip_badge         {"vip"};
const String founder_badge     {"founder"};
const String subscriber_badge  {"subscriber"};

String CLIENT_ID         {};
String BROADCASTER_ID    {};
String AUTH_TOKEN        {};
String YOUTUBE_API_KEY   {};
String BROADCASTER_NAME  {};
String TIKTOK_SESSION_ID {};

struct Parsed_Message
{
    String host;
    String nick;
    String message;
    String user_id;
    String badges;
    bool reply;
};

bool roll_dice(const int n)
{
    std::uniform_int_distribution dis{1, 100};
    return dis(GENERATOR) <= n;
}

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

String tts_text_format(const String& data)
{
    auto vec {tokenize(data)};
    String result;
    for(auto& s : vec){
        result += s + '+';
    }
    result.pop_back();
    return result;
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

    auto get_message_start {[&]()
    {
        auto fc {msg.find(" :")};
        auto colon {msg.find(" :", fc + 2)};
        return colon + 2;
    }};

    auto get_message {[&]()
    {
        return msg.substr(get_message_start(), msg.npos);
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
    {
        auto r {msg.find("reply")};
        result.reply = r != String::npos && r < get_message_start();
    }

    clean_line(&result.host);
    clean_line(&result.nick);
    clean_line(&result.message);
    clean_line(&result.badges);
    clean_line(&result.user_id);

    return result;
}

void twitch_irc_message_handler(Connection_Metadata* m, Client::message_ptr msg)
{
    std::scoped_lock g {m->message_mutex};
    auto pay_load {msg->get_payload()};
    if(msg->get_opcode() == websocketpp::frame::opcode::text){
        m->messages.push_back(pay_load);
    }
    else{
        pay_load = websocketpp::utility::to_hex(pay_load);
    }
    printf("%s : %s\n", m->name.c_str(), pay_load.c_str());
}

void event_sub_message_handler(Connection_Metadata* m, Client::message_ptr msg)
{
    std::scoped_lock g {m->message_mutex};
    auto pay_load {msg->get_payload()};
    if(msg->get_opcode() != websocketpp::frame::opcode::text){
        pay_load = websocketpp::utility::to_hex(pay_load);
    }
    m->messages.push_back("<< " + pay_load);
    printf("%s : %s\n", m->name.c_str(), pay_load.c_str());
}

struct User
{
    String user_id    {""};
    String user_login {""};

    String last_known_nick {};
    String last_known_badges;

    s64 points        {0}; 
    s64 gamba_points  {0};
    s64 social_credit {0};

    bool stronger(const User& b)
    {
        if(!last_known_badges.empty() && b.last_known_badges.empty()){
            return true;
        }
        if(last_known_badges.find(broadcaster_badge) != String::npos){
            return true;
        }
        if(last_known_badges.find(moderator_badge) != String::npos && b.last_known_badges.find(moderator_badge) == String::npos){
            return true;
        }
        if(last_known_badges.find(vip_badge) != String::npos && b.last_known_badges.find(vip_badge) == String::npos){
            return true;
        }
        if(last_known_badges.find(subscriber_badge) != String::npos && b.last_known_badges.find(subscriber_badge) == String::npos){
            return true;
        }
        return false;
    }

    void serialize_out(std::ostream* data)
    {
        auto& i {*data};
        i<<"User : " <<user_id<<"\n\n";
        i<<"\tPoints : " <<points<<"\n\n";
        i<<"\tGamba_Points : " <<gamba_points<<"\n\n";
        i<<"\tSocial_Credit : " <<social_credit<<"\n\n";
    }
};

struct Bot
{

    struct Voice;
    struct Sound_To_Play;

    struct Sound_Effect
    {
        String name;
        std::function<void(Sound_Effect*, Sound_To_Play*)> callback;
        s16 angle {0};
    };

    struct Music_Info
    {
        Youtube_Video_Info video;
        String video_link;
        Vector<String> args;
    };

    struct Sound
    {
        String name;
        Mix_Chunk* chunk {nullptr};
    };

    struct Sound_To_Play
    {
        Timer pause;
        bool played {false};
        int channel;
        Sound* sound {nullptr};
        Mix_Chunk* tts {nullptr};
        int loops {0};
        int volume {MIX_MAX_VOLUME / 4};
        s64 tts_id {-1};

        void play()
        {
            played = true;
            if(sound){
                channel = Mix_PlayChannel(-1, sound->chunk, loops);
            }
            else{
                channel = Mix_PlayChannel(-1, tts, loops);
            }
            Mix_Volume(channel, volume);

            for(auto& e : sound_effects){
                e.callback(&e, this);
            }
        }
        void clean_up()
        {
            if(tts){
                Mix_FreeChunk(tts);
            }
            Mix_UnregisterAllEffects(channel);
        }

        Vector<Sound_Effect> sound_effects;
    };

    using Callback = void(*)(Bot*, const String& user_id, const Vector<String>& args);

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
        std::scoped_lock g {generic_mutex};
        {
            std::scoped_lock g {event_sub_handle->message_mutex};
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

                    if(s == "notification")
                    {
                        s = json_get_value_naive("subscription_type", data);
                        String message;
                        if(s == "channel.follow")
                        {
                            s = json_get_value_naive("user_name", data);
                            auto user_id {json_get_value_naive("user_id", data)};
                            auto found {false};
                            for(auto& u : already_thanks_for_the_follow)
                            {
                                if(u == user_id)
                                {
                                    found = true;
                                    break;
                                }
                            }
                            if(!found)
                            {
                                for(auto& u : users)
                                {
                                    if(u.user_id == user_id)
                                    {
                                        found = true;
                                        break;
                                    }
                                }
                            }
                            if(!found)
                            {
                                add_message(format_reply_2("Yo lil bro! Thanks for the follow!", s));
                                message = tts_text_format(s + " thanks for the follow lil bro!");

                                already_thanks_for_the_follow.push_back(user_id);
                                users.push_back({user_id});
                            }
                        }
                        else if(s == "channel.subscribe" || s == "channel.subscription.message")
                        {
                            s = json_get_value_naive("user_name", data);
                            add_message(format_reply_2("Yo lil bro! Thanks for subbing!", s));
                            message = tts_text_format(s + " thanks for the subbing lil bro!");
                        }
                        if(!message.empty())
                        {
                            auto thank_you {tts_from_streamelements("Brian", message)};
                            if(thank_you.tts)
                            {
                                std::scoped_lock g {sound_mutex};
                                sounds_to_play.push_back(thank_you);
                            }
                        }
                    }
                }
            }
        } 
        {
            std::scoped_lock g {handle->message_mutex};
            for(auto& m : handle->messages)
            {
                if(m.find("JOIN") != String::npos){
                    handle->joined = true;
                }
                else
                {
                    auto parsed_message {parse_message(m)};
                    if(!parsed_message.message.empty()){
                        priv_messages.push_back(parsed_message);
                    }
                    else
                    {
                        auto tokens {tokenize(m)};
                        if(!tokens.empty())
                        {
                            if(tokens[0] == "PING"){
                                ping_messages.push_back(m);
                            }
                        }
                    }
                }
            }
            handle->messages.clear();
        }
        if(!priv_messages.empty())
        {
            const auto& msg {priv_messages.front()};
            auto tokens {tokenize(msg.message)};
            String to_who;
            if(msg.reply)
            {
                to_who = tokens.front();
                tokens.erase(tokens.begin());
            }
            {
                auto u {get_user(msg.user_id)};
                if(u)
                {
                    u->last_known_badges = msg.badges;
                    u->last_known_nick = msg.nick;
                }
            }
            auto has_command {false};
            if(tokens[0][0] == '!')
            {
                auto c {find_command(tokens[0].substr(1, String::npos))};
                if(c)
                {
                    has_command = true;
                    bool badge_is_good {};
                    if(c->no_badges){
                        badge_is_good = msg.badges.empty(); 
                    }
                    else
                    {
                        badge_is_good = c->badges.empty();
                        for(const auto& s : c->badges)
                        {
                            if(msg.badges.find(s) != String::npos)
                            {
                                badge_is_good = true;
                                break;
                            }
                        }
                    }
                    if(badge_is_good)
                    {
                        tokens.erase(tokens.begin());
                        tokens.insert(tokens.begin(), msg.nick);
                        if(c->name == "tts" && !to_who.empty()){
                            tokens.insert(tokens.begin() + 1, to_who);
                        }
                        auto t {std::thread(c->callback, this, msg.user_id, tokens)};
                        t.detach();
                    }
                    else{
                        add_message(format_reply(msg.nick, "You are not BatChest enough!"));
                    }
                }
            }
            if(!has_command)
            {
                String str;
                for(const auto& s : tokens)
                {
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
                    ban_user(msg.user_id, duration);
                }
            }
            for(const auto& s : tokens)
            {
                if(s == "BatChest"){
                    batchest_count++;
                }
            }
            priv_messages.erase(priv_messages.begin());
        }
        if(!ping_messages.empty())
        {
            auto& pong {ping_messages.front()};
            pong[1] = 'O';
            add_message(pong);
            printf("%s\n", pong.c_str());
            ping_messages.erase(ping_messages.begin());
        }
    }

    void check_music_queue()
    {
        std::scoped_lock g {music_mutex};
        if(!music_queue.empty())
        {
            const auto can_play {!music_playing()};
            if(can_play)
            {
                const auto& music {music_queue.front()};
    
                String str {"start /min mpv " + music.video_link + " --no-video"};
    
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
        std::scoped_lock g {sound_mutex};
        if(!sounds_to_play.empty())
        {
            auto& s {sounds_to_play.front()};
            if(s.pause.wait > 0)
            {
                if(!s.pause.started){
                    s.pause.start();
                }
                else if(s.pause.is_time()){
                    sounds_to_play.erase(sounds_to_play.begin());
                }
            }
            else
            {
                if(!s.played){
                    s.play();
                }
                else
                {
                    if(!Mix_Playing(s.channel))
                    {
                        s.clean_up();
                        sounds_to_play.erase(sounds_to_play.begin());
                    }
                }
            }
        }
        if(!tts_sounds_to_play_elevated.empty())
        {
            auto& s {tts_sounds_to_play_elevated.front()};
            if(!Mix_Playing(s.channel))
            {
                s.clean_up();
                tts_sounds_to_play_elevated.erase(tts_sounds_to_play_elevated.begin());
            }
        }
    }

    Sound* get_sound(const String& s)
    {
        for(auto& sound : sounds)
        {
            if(sound.name == s){
                return &sound;
            }
        }
        return nullptr;
    }

    const Sound_Effect* get_sound_effect(const String& s)
    {
        for(auto& e : sound_effects)
        {
            if(e.name == s){
                return &e;
            }
        }
        return nullptr;
    }

    Sound_To_Play get_tts(const Voice* voice, const String& phrase)
    {
        if(!voice){
            return tts_from_streamelements("brian", phrase);
        }
        else
        {
            if(voice->api == Voice::Stream_Elements){
                return tts_from_streamelements(voice->name, phrase);
            }
            else{
                return tts_from_tiktok(voice->code, phrase);
            }
        }
    }

    Sound_To_Play tts_from_streamelements(String voice, const String& phrase)
    {
        std::scoped_lock gg {curl_mutex};
        curl_easy_reset(curl_handle);
        Sound_To_Play play;
        voice[0] = toupper(voice[0]);
        String url {"https://api.streamelements.com/kappa/v2/speech?voice=" + voice + "&text="};
        const String data {curl_call(url + phrase, curl_handle)};

        auto rwops {SDL_RWFromConstMem((void*)data.data(), data.length())};
        if(rwops)
        {
            play.tts = Mix_LoadWAV_RW(rwops, 0);
            SDL_RWclose(rwops);
        }
        return play;
    };

    Sound_To_Play tts_from_tiktok(String voice, const String& phrase)
    {
        std::scoped_lock gg {curl_mutex};
        curl_easy_reset(curl_handle);
        Sound_To_Play play;

        auto list {set_curl_headers((String{"User-Agent"} + ":" + " com.zhiliaoapp.musically/2022600030 (Linux; U; Android 7.1.2; es_ES; SM-G988N; Build/NRD90M;tt-ok/3.12.13.1)").c_str(),
                                     (String{"Cookie"} + ":" + " sessionid=" + TIKTOK_SESSION_ID).c_str(), 
                                     (String{"Content-Length"} + ":" + "0").c_str())};

        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "POST");

        String url {"https://api22-normal-c-useast1a.tiktokv.com/media/api/text/speech/invoke/?text_speaker=" + voice + "&req_text=" + phrase + "&speaker_map_type=0&aid=1233"};

        const String data {curl_call(url, curl_handle)};

        auto b64 {json_get_value_naive("v_str", data)};

        auto file {websocketpp::base64_decode(b64)};

        auto rwops {SDL_RWFromConstMem((void*)file.data(), file.length())};
        if(rwops)
        {
            play.tts = Mix_LoadWAV_RW(rwops, 0);
            SDL_RWclose(rwops);
        }

        curl_slist_free_all(list);
        return play;
    };

    Voice* has_voice(const String s)
    {
        for(auto& v : voices)
        {
            if(v.name == s){
                return &v;
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
        std::scoped_lock g {send_mutex};
        for(const auto& s : messages_to_send){
            end_point.send(connection_id, s);
        }
        messages_to_send.clear();
    }

    void add_message(const String& str)
    {
        std::scoped_lock g {send_mutex};
        messages_to_send.push_back(str);
    }

    User* get_user(const String& id)
    {
        for(auto& u : users)
        {
            if(u.user_id == id){
                return &u;
            }
        }
        return nullptr;
    }
    User* get_user_by_nick(const String& nick)
    {
        if(nick.empty()){
            return nullptr;
        }
        for(auto& u : users)
        {
            if(u.last_known_nick == nick){
                return &u;
            }
        }
        return nullptr;
    }

    void ban_user(const String& id, const int dur)
    {
        std::scoped_lock guard {curl_mutex};
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
        std::scoped_lock guard {curl_mutex};
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

    void build_followers_list()
    {
        std::scoped_lock g {curl_mutex};
        curl_easy_reset(curl_handle);
        auto list {set_curl_headers(("Authorization: Bearer " + AUTH_TOKEN).c_str(),
                                ("Client-Id: " + CLIENT_ID).c_str())};
        
        String url {"https://api.twitch.tv/helix/channels/followers?broadcaster_id=" + BROADCASTER_ID + "&first=100"};
        auto s {curl_call(url, curl_handle, list)};
        users.reserve(5000);

        users.push_back({BROADCASTER_ID});

        while(true)
        {
            auto i {s.find("user_id")};
            if(i != String::npos)
            {
                users.push_back({});
                auto& u {users.back()};
                u.user_id = json_get_value_naive("user_id", s);
                u.user_login = json_get_value_naive("user_login", s);
                s.erase(0, s.find("}", i) + 1);
                if(s.empty()){
                    break;
                }
            }
            else
            {
                auto cursor {json_get_value_naive("cursor", s)};
                if(!cursor.empty()){
                    s = curl_call(url + "&after=" + cursor, curl_handle, list);
                }
                else{
                    break;
                }
            }
        }
        curl_slist_free_all(list);

        if(users.empty()){
            return;
        }

        const auto file {"followers.txt"};

        Vector<User> temp_users;

        String line;
        String tag;
        String value;
        {
            {
                std::ifstream o {file};
                User* current {nullptr};
                while(std::getline(o, line))
                {
                    clean_line(&line);
                    if(line != "End")
                    {
                        if(!line.empty())
                        {
                            extract_tag_and_value_from_line(line, &tag, &value);
                            if(tag == "User")
                            {
                                temp_users.push_back({});
                                current = &temp_users.back();
                                current->user_id = value;
                            }
                            else if(tag == "Points"){
                                current->points = string_to_int(value);
                            }
                            else if(tag == "Gamba_Points"){
                                current->gamba_points = string_to_int(value);
                            }
                            else if(tag == "Social_Credit"){
                                current->social_credit = string_to_int(value);
                            }
                        }
                    }
                }
            }
            std::ofstream f {file};
            for(auto& u : users)
            {
                User* data {nullptr};
                for(auto& j : temp_users)
                {
                    if(u.user_id == j.user_id)
                    {
                        u = j;
                        break;
                    }
                }
                u.serialize_out(&f);
            }
        }
    }
    
    void serialize_in()
    {
        String line;
        String tag;
        String value;
        {
            std::ifstream file {data_file_name};
            while(std::getline(file, line))
            {
                clean_line(&line);
                if(!line.empty())
                {
                    extract_tag_and_value_from_line(line, &tag, &value);
                    if(tag == "BatChest_Count")
                    {
                        std::stringstream ss {value};
                        ss>>batchest_count;
                    }
                }
            }
        }

        {
            std::ifstream file {"banned_words.txt"};
            Pair<String, int> p;
            while(file>>p.first>>p.second){
                banned_words.push_back(p);
            }
        }

        {
            std::ifstream file {already_followed};
            while(std::getline(file, line))
            {
                clean_line(&line);
                already_thanks_for_the_follow.push_back(line);
            }
        }

        {
            printf("loading sounds\n");
            const String directory {"sounds/"};
            String file_name;

            Vector<Pair<String, String>> path_and_name;
            path_and_name.reserve(200);

            for(const auto& e : Files::directory_iterator(directory))
            {
                file_name = e.path().filename().string(); 

                path_and_name.push_back({directory + file_name, e.path().filename().stem().string()});
            }

            sounds.resize(path_and_name.size());

            auto sound_counter {0};
            auto load_sound {[&](const Pair<String, String>* data, const int start, const int end)
            {
                for(int i = start; i < end; i++)
                {
                    const auto& d {data[i]};
                    auto chunk {Mix_LoadWAV(d.first.c_str())};
                    sound_counter++;
                    auto& s {sounds[i]};
                    s.name = d.second;
                    s.chunk = chunk;
                }
            }};

            const int data_to_handle {25};
            auto number_of_threads {path_and_name.size() / data_to_handle};
            const auto rem {path_and_name.size() % data_to_handle};

            if(number_of_threads < 1){
                number_of_threads = 1;
            }
            if(rem != 0){
                number_of_threads++;
            }

            Vector<std::thread> threads(number_of_threads);
            for(int i = 0; i < threads.size(); i++)
            {
                auto start {i * data_to_handle};

                int end {start + data_to_handle};
                if(end > path_and_name.size()){
                    end = path_and_name.size();
                }
                threads[i] = std::thread(load_sound, path_and_name.data(), start, end);
            }
            for(auto& t : threads){
                t.join();
            }
            if(sound_counter != path_and_name.size())
            {
                printf("big problem lil bro %i %llu\n", sound_counter, path_and_name.size());
                assert(false);
            }
            printf("finished loading sounds\n");
        }
    
    }

    void save_data()
    {
        std::scoped_lock g {generic_mutex};
        {
            std::ofstream file {already_followed};
            for(auto& u : already_thanks_for_the_follow){
                file<<u<<"\n";
            }
        }

        {
            std::ofstream file {"followers.txt"};
            for(auto& u : users){
                u.serialize_out(&file);
            }
        }

        {
            std::ofstream file {data_file_name};
            file<<"BatChest_Count : "<<batchest_count<<"\n\n";
        }
    }

    void quit()
    {
        for(auto& s : sounds)
        {
            if(s.chunk){
                Mix_FreeChunk(s.chunk);
            }
        }
    }

    s64 TTS_ID_COUNTER {0};

    bool experimental {false};

    int connection_id;
    int event_sub_connection_id;
    Connection_Metadata* handle;
    Connection_Metadata* event_sub_handle;
    Websocket_Endpoint end_point;
    Stamp last_music_stamp;
    std::thread music_thread;

    Vector<Command> commands;
    Vector<String> messages_to_send;

    u64 batchest_count {0};
    String today;
    String event_sub_session_id;


    struct Voice
    {
        enum API
        {
            Stream_Elements,
            TikTok,
        };
        String name;
        API api;
        String code;
    };

    Vector<Voice> voices;

    Vector<Sound> sounds;
    Vector<Sound_To_Play> sounds_to_play;
    Vector<Sound_To_Play> tts_sounds_to_play_elevated;
    Vector<String> already_thanks_for_the_follow;

    Vector<Sound_Effect> sound_effects;

    Music_Info last_song;
    Vector<Music_Info> music_queue;
    Vector<Pair<String, int>> banned_words;
    Vector<User> users;

    Vector<Parsed_Message> priv_messages;
    Vector<String> ping_messages;

    String already_followed {"already_followed.txt"};
    String data_file_name   {"bot.txt"};

    std::mutex generic_mutex;
    std::mutex sound_mutex;
    std::mutex music_mutex;
    std::mutex send_mutex;
    std::mutex curl_mutex;

};

void hug_callback(Bot* b, const String& id, const Vector<String>& args)
{
    if(args.size() == 1){
        b->add_message(format_reply(args[0], "hug deez nuts GOTTEM"));
    }
    else
    {
        if(roll_dice(50)){
            b->add_message(format_send("@" + args[0] + " is hugging @" + args[1] + " HUGGIES"));
        }
        else{
            b->add_message(format_reply(args[0], "hug deez nuts GOTTEM"));
        }
    }
}

void vanish_callback(Bot* b, const String& id, const Vector<String>& args)
{
    if(args.size() == 1){
        b->ban_user(id, -1);
    }
    else{
        b->ban_user(id, string_to_int(args[1]));
    }
}

void bully_callback(Bot* bot, const String& id, const Vector<String>& args)
{
    // TODO make this better add slap sound or something
    if(args.size() == 1 ){
        bot->add_message(format_reply(args[0], "slap deez nuts GOTTEM"));
        return;
    }
    std::scoped_lock g {bot->generic_mutex};
    auto a {bot->get_user(id)};
    auto b {bot->get_user_by_nick(args[1])};
    if(a && b)
    {
        if(a->stronger(*b)){
            bot->add_message(format_send("@" + args[0] + " slapped " + "@" + args[1]));
        }
        else{
            bot->add_message(format_send("@" + args[1] + " slapped " + "@" + args[0]));
        }
    }
    else{
        return;
    }
}

void gamba_callback(Bot* b, const String& id, const Vector<String>& args)
{
    std::scoped_lock g {b->generic_mutex};
    auto u {b->get_user(id)};
    if(u)
    {
        if(args.size() == 1){
            b->add_message(format_reply(args[0], "you have " + std::to_string(u->gamba_points) + " points"));
        }
        else
        {
            s64 points;
            auto reward_factor {1};
            if(args[1] == "all")
            {
                points = u->gamba_points;
                reward_factor = 10;
            }
            else if(args[1] == "half")
            {
                points = u->gamba_points / 2;
                reward_factor = 5;
            }
            else{
                points = string_to_int<s64>(args[1]);
            }

            if(points > 0)
            {
                if(u->gamba_points < points){
                    b->add_message(format_reply(args[0], "not enough points"));
                }
                else
                {
                    auto string_points {std::to_string(points)};
                    b->add_message(format_reply(args[0], "rolling dice nuts in your mouth GAMBA points at risk is " + string_points));
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

                    if(roll_dice(50))
                    {
                        u->gamba_points += points * reward_factor;
                        b->add_message(format_reply(args[0], "you won " + std::to_string(points * reward_factor) + " GAMBA you have " + std::to_string(u->gamba_points) + " points!"));
                    }
                    else
                    {
                        u->gamba_points -= points;
                        b->add_message(format_reply(args[0], "you lost " + string_points + " pepeLost noob you have " + std::to_string(u->gamba_points) + " points!"));
                    }
                }
            }
        }

        if(u->gamba_points <= 0)
        {
            u->gamba_points = 500;
            b->add_message(format_reply(args[0], "noob since you're so poor the gods have blessed you with 500 points GAMBAADDICT"));
        }

    }
}

void commands_callback(Bot* b, const String& id, const Vector<String>& args)
{
    String list;
    for(auto& c : b->commands){
        list += c.name + " "; 
    }
    b->add_message(format_reply(args[0], list));
}

void stack_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "stack deez nuts in your mouth GOTTEM"));
}

void drop_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "drop deez nuts in your mouth GOTTEM"));
}

void discord_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "join my discord https://discord.gg/9xVKDekJtg"));
}

void game_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Steam : working on a roguelike called Morphus! https://store.steampowered.com/app/2371310/Morphus/"));
}

void demo_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Demo : Check out the demo here! https://store.steampowered.com/app/2371310/Morphus/"));
}

void editor_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Editor : neovim baseg"));
}

void font_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Font : https://github.com/nathco/Office-Code-Pro"));
}

void keyboard_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Keyboard : keychron k8 + kailh white switches"));
}

void engine_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Engine : c++ and SDL2"));
}

void vimconfig_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Vim Config : https://github.com/mrnoob17/my-nvim-init/blob/main/init.lua"));
}

void friends_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "Friends : twitch.tv/azenris twitch.tv/tk_dev twitch.tv/tkap1 twitch.tv/athano twitch.tv/cakez77 twitch.tv/tapir2342"));
}

void os_callback(Bot* b, const String& id, const Vector<String>& args)
{
   b->add_message(format_reply(args[0], "OS : not linux baseg"));
}

void tts_callback(Bot* b, const String& id, const Vector<String>& args)
{
    // TODO parser needs some work still
    std::scoped_lock g {b->sound_mutex};
    String global_phrase {};

    Bot::Voice* global_voice {nullptr};
    const auto tts_id {b->TTS_ID_COUNTER};
    const auto previous_size {b->sounds_to_play.size()};
    b->TTS_ID_COUNTER++;

    Vector<Bot::Sound_Effect> current_sound_effects;
    auto last_global_token {0};

    for(int i = 1; i < args.size(); i++)
    {
        const auto& s {args[i]};

        if(s[0] == '-' || s[0] == '+')
        {
            auto pipe {s.find('|')};
            auto pipe2 {s.find('|', pipe + 1)};

            String stem;

            if(s.length() == 1)
            {
                if(s[0] == '-'){
                    stem = "minus";
                }
                else{
                    stem = "plus";
                }
            }
            else
            {
                if(s.substr(0, 2) == "-p")
                {
                    if(s.length() > 2)
                    {
                        Bot::Sound_To_Play play;
                        play.pause.wait = string_to_float(s.substr(2, String::npos));
                        b->sounds_to_play.push_back(play);
                    }
                    continue;
                }
                else{
                    stem = s.substr(1, pipe - 1);
                }
            }

            auto se {b->get_sound_effect(stem)};
            if(se)
            {
                //if(!global_phrase.empty() && last_token_index < i){
                //    current_sound_effects.clear();
                //}
                //current_sound_effects.push_back(*se);
                //auto& see {current_sound_effects.back()};
                //if(stem == "a")
                //{
                //    if(pipe != s.length() - 1){
                //        see.angle = string_to_int(s.substr(pipe + 1, String::npos)); 
                //    }
                //}
                //last_token_index = i;
            }
            else
            {
                Bot::Sound_To_Play play;

                auto v {b->has_voice(stem)};
                if(v)
                {
                    String phrase {};
                    int j;
                    for(j = i + 1; j < args.size(); j++)
                    {
                        // TODO if valid sound or voice continue else keep adding to the phrase

                        if(args[j][0] == '-' || args[j][0] == '+')
                        {
                            i = j - 1;
                            break;
                        }
                        phrase += args[j];
                        if(j != args.size() - 1){
                            phrase += '+';
                        }
                    }
                    if(j == args.size()){
                        i = j;
                    }
                    current_sound_effects.clear();
                    play = b->get_tts(v, phrase);
                    if(last_global_token > i){
                        global_voice = v;
                    }
                }
                else
                {
                    auto sound {b->get_sound(stem)};
                    if(sound){
                        play.sound = sound;
                    }
                    else{
                        global_phrase += stem + '+';
                    }
                }
                if(play.sound || play.tts)
                {
                    // TODO
                    if(!global_phrase.empty())
                    {
                        global_phrase.pop_back();
                        auto tts {b->get_tts(global_voice, global_phrase)};
                        if(tts.tts)
                        {
                            tts.sound_effects = current_sound_effects;
                            b->sounds_to_play.push_back(tts);
                        }
                        global_phrase.clear();
                    }
                    play.sound_effects = current_sound_effects;
                    if(play.sound){
                        current_sound_effects.clear();
                    }
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
                    if(pipe2 != String::npos && pipe2 != s.size() - 1)
                    {
                        play.loops = string_to_int(s.substr(pipe2 + 1, String::npos));
                        if(play.loops > 5){
                            play.loops = 5;
                        }
                        else if(play.loops < 0){
                            play.loops = 0;
                        }
                    }
                    if(s[0] == '-'){
                        b->sounds_to_play.push_back(play);
                    }
                    else
                    {
                        play.loops = 0;
                        play.play();
                        b->tts_sounds_to_play_elevated.push_back(play);
                    }
                }
            }
        }
        else
        {
            global_phrase += s + '+';
            last_global_token = i;
        }
    }
    if(!global_phrase.empty())
    {
        global_phrase.pop_back();
        auto tts {b->get_tts(global_voice, global_phrase)};
        if(tts.tts)
        {
            tts.sound_effects = current_sound_effects;
            b->sounds_to_play.push_back(tts);
        }
    }
    for(int i = previous_size; i < b->sounds_to_play.size(); i++){
        b->sounds_to_play[i].tts_id = tts_id;
    }
}

void music_callback(Bot* b, const String& id, const Vector<String>& args)
{
    auto video_link {args[1]};
    std::scoped_lock guard {b->curl_mutex};

    {
        auto list {video_link.find("&list")};
        if(list != String::npos){
            video_link.erase(list, String::npos);
        }
        list = video_link.find("&ab_channel");
        if(list != String::npos){
            video_link.erase(list, String::npos);
        }
    }

    auto equals {video_link.find('=')};
    if(equals == String::npos){
        b->add_message(format_reply(args[0], "AwkwardMonkey invalid video"));
        return;
    }

    auto video       {video_link.substr(equals + 1, String::npos)}; 
    String video_arg {"&id=" + video};
    String key_arg   {"&key=" + YOUTUBE_API_KEY};
    String api       {"https://www.googleapis.com/youtube/v3/videos?"};
    String part      {"part=snippet,contentDetails,statistics"};

    curl_easy_reset(curl_handle);

    auto curl_result {curl_call(api + part + video_arg + key_arg, curl_handle)};

    auto yt_video {parse_youtube_api_result(curl_result)};

    if(yt_video.duration > 600)
    {
        b->add_message(format_reply(args[0], "AwkwardMonkey video too long max is 600 secs"));
        return;
    }
    else if(yt_video.title.empty())
    {
        b->add_message(format_reply(args[0], "AwkwardMonkey invalid video"));
        return;
    }
    else if(yt_video.like_count < 1000)
    {
        b->add_message(format_reply(args[0], "Susge video like count must be >= 1000"));
        return;
    }
    else if(yt_video.view_count < 1000)
    {
        b->add_message(format_reply(args[0], "Susge video view count must be >= 1000"));
        return;
    }

    b->add_message(format_reply(args[0], "FeelsOkayMan song added to the queue"));
    std::scoped_lock g {b->music_mutex};

    b->music_queue.push_back({yt_video, video_link, args});
}

void skip_song_callback(Bot* b, const String& id, const Vector<String>& args)
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

void skip_sound_callback(Bot* b, const String& id, const Vector<String>& args)
{
    std::scoped_lock g {b->sound_mutex};
    if(!b->sounds_to_play.empty())
    {
        const auto id {b->sounds_to_play.front().tts_id};
        while(b->sounds_to_play.front().tts_id == id)
        {
            Mix_HaltChannel(b->sounds_to_play.front().channel);
            b->sounds_to_play.erase(b->sounds_to_play.begin());
            if(b->sounds_to_play.empty()){
                break;
            }
        }
    }
}

void music_count_callback(Bot* b, const String& id, const Vector<String>& args)
{
    std::scoped_lock g {b->music_mutex};
    b->add_message(format_reply(args[0], std::to_string(b->music_queue.size()) + " song(s) in the queue"));
}

void song_callback(Bot* b, const String& id, const Vector<String>& args)
{
    if(b->music_playing()){
        b->add_message(format_reply(args[0], "Song : " + b->last_song.video.title));
    }
}

void bot_callback(Bot* b, const String& id, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "C++ Twitch Bot : https://github.com/mrnoob17/twitch_bot"));
}

void batchest_callback(Bot* b, const String& id, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "BatChest Count : " + std::to_string(b->batchest_count)));
}

void today_callback(Bot* b, const String& id, const Vector<String>& args)
{
    if(!b->today.empty()){
        b->add_message(format_reply(args[0], "Today : " + b->today));
    }
}

void set_today_callback(Bot* b, const String& id, const Vector<String>& args)
{
    b->today = "";
    for(int i = 1; i < args.size(); i++){
        b->today += args[i] + " ";
    }
}

void set_title_callback(Bot* b, const String& id, const Vector<String>& args)
{
    std::scoped_lock guard {b->curl_mutex};
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

void founder_callback(Bot* b, const String& id, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "You're a founder... founder of deez nuts! GOTTEM"));
}

void mod_callback(Bot* b, const String& id, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "You're a moderator, protector of deez nuts GOTTEM"));
}

void pleb_callback(Bot* b, const String& id, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "You're a pleb, cultivator of deez nuts GOTTEM"));
}

void sub_callback(Bot* b, const String& id, const Vector<String>& args)
{
    b->add_message(format_reply(args[0], "You're a subscriber, nourisher of deez nuts GOTTEM"));
}

void start_bot(Bot* _bot, int args, const char** argc)
{
    BROADCASTER_NAME = argc[1];

    auto& bot {*_bot};
    {
        std::ifstream file {"token.nut"};
        file>>CLIENT_ID;
        file>>AUTH_TOKEN;
        file>>YOUTUBE_API_KEY;
        file>>TIKTOK_SESSION_ID;
    }

    {
        curl_easy_reset(curl_handle);
        auto list {set_curl_headers(("Authorization: Bearer " + AUTH_TOKEN).c_str(),
                                    ("Client-Id: " + CLIENT_ID).c_str())};

        String s {curl_call(("https://api.twitch.tv/helix/users?login=" + BROADCASTER_NAME).c_str(), curl_handle, list)};
        BROADCASTER_ID = json_get_value_naive("id", s);
        curl_slist_free_all(list);
    }


    bot.experimental = false;
    bot.serialize_in();

    bot.voices = {{"brian", Bot::Voice::Stream_Elements},
                  {"filiz", Bot::Voice::Stream_Elements},
                  {"astrid", Bot::Voice::Stream_Elements},
                  {"tatyana", Bot::Voice::Stream_Elements},
                  {"maxim", Bot::Voice::Stream_Elements},
                  {"carmen", Bot::Voice::Stream_Elements},
                  {"ines", Bot::Voice::Stream_Elements},
                  {"cristiano", Bot::Voice::Stream_Elements},
                  {"vitoria", Bot::Voice::Stream_Elements},
                  {"ricardo", Bot::Voice::Stream_Elements},
                  {"jp", Bot::Voice::TikTok, "jp_005"},
                  {"mjp", Bot::Voice::TikTok, "jp_006"},
                  {"stitch", Bot::Voice::TikTok, "en_us_stitch"},
                  {"rocket", Bot::Voice::TikTok, "en_us_rocket"},
                  {"cp", Bot::Voice::TikTok, "en_us_c3po"},
                  {"pirate", Bot::Voice::TikTok, "en_male_pirate"},
                  {"de", Bot::Voice::TikTok, "de_001"},
                  {"chip", Bot::Voice::TikTok, "en_male_m2_xhxs_m03_silly"},
                  {"betty", Bot::Voice::TikTok, "en_female_betty"},
                  {"granny", Bot::Voice::TikTok, "en_female_grandma"},
                  {"soy", Bot::Voice::TikTok, "en_male_ukneighbor"},
                  {"butler", Bot::Voice::TikTok, "en_male_ukbutler"},
                  {"mde", Bot::Voice::TikTok, "de_002"},
                  {"es", Bot::Voice::TikTok, "es_femalte_f6"},
                  {"mes", Bot::Voice::TikTok, "es_002"},
                  {"br", Bot::Voice::TikTok, "br_001"},
                  {"mbr", Bot::Voice::TikTok, "br_005"},
                  {"au", Bot::Voice::TikTok, "en_au_001"},
                  {"mau", Bot::Voice::TikTok, "en_au_002"},
                  {"us", Bot::Voice::TikTok, "en_us_001"},
                  {"mus", Bot::Voice::TikTok, "en_us_006"},
                  {"fr", Bot::Voice::TikTok, "fr_001"},
                  {"fr2", Bot::Voice::TikTok, "fr_002"},
                  {"sing", Bot::Voice::TikTok, "en_female_f08_salut_damour"},
                  {"sing2", Bot::Voice::TikTok, "en_male_m03_lobby"},
                  {"sing3", Bot::Voice::TikTok, "en_female_f08_warmy_breeze"},
                  {"sing4", Bot::Voice::TikTok, "en_male_m03_sunshine_soon"},
                  {"nar", Bot::Voice::TikTok, "en_male_narration"},
                  {"fun", Bot::Voice::TikTok, "en_male_funny"},
                  {"emo", Bot::Voice::TikTok, "en_female_emotional"}};

    //bot.sound_effects.push_back({"r", [](Bot::Sound_Effect*, Bot::Sound_To_Play* s){
    //    Mix_SetReverseStereo(s->channel, 1);
    //}});

    //bot.sound_effects.push_back({"a", [](Bot::Sound_Effect* e, Bot::Sound_To_Play* s){
    //    Mix_SetPosition(s->channel, e->angle, 0);
    //}});

    bot.add_command("commands", commands_callback); 
    bot.add_command("stack", stack_callback); 
    bot.add_command("drop", drop_callback); 
    bot.add_command("discord", discord_callback); 
    bot.add_command("game", game_callback); 
    bot.add_command("demo", demo_callback); 
    bot.add_command("editor", editor_callback); 
    bot.add_command("engine", engine_callback); 
    bot.add_command("font", font_callback); 
    bot.add_command("keyboard", keyboard_callback); 
    bot.add_command("vimconfig", vimconfig_callback); 
    bot.add_command("friends", friends_callback); 
    bot.add_command("gamba", gamba_callback); 
    bot.add_command("slap", bully_callback); 
    bot.add_command("vanish", vanish_callback); 
    bot.add_command("hug", hug_callback); 
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

    bot.connection_id = bot.end_point.connect("wss://irc-ws.chat.twitch.tv:443", "Twitch IRC", twitch_irc_message_handler);
    bot.event_sub_connection_id = bot.end_point.connect("wss://eventsub.wss.twitch.tv/ws", "Event Sub", event_sub_message_handler);

    printf("%i\n", bot.connection_id);
    printf("%i\n", bot.event_sub_connection_id);
    if(bot.connection_id == -1 || bot.event_sub_connection_id == -1){
        printf("connection failed");
    }
    else
    {
        bot.handle = bot.end_point.get_metadata(bot.connection_id);

        {
            std::unique_lock l {bot.handle->opened_mutex};
            bot.handle->opened_condition_variable.wait(l, [&]{
                return bot.handle->opened;
            });
        }

        bot.end_point.send(bot.connection_id, "CAP REQ :twitch.tv/membership twitch.tv/tags twitch.tv/commands\r\n");
        bot.end_point.send(bot.connection_id, "PASS oauth:" + AUTH_TOKEN + "\r\n");
        bot.end_point.send(bot.connection_id, "NICK " + BROADCASTER_NAME + "\r\n");
        bot.end_point.send(bot.connection_id, "JOIN #" + BROADCASTER_NAME + "\r\n");

        bot.event_sub_handle = bot.end_point.get_metadata(bot.event_sub_connection_id);

        bot.build_followers_list();

        std::random_device random_device;

        GENERATOR = std::knuth_b {random_device()};
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

        auto sound_thread {std::thread([&](Bot* b)
        {
            while(true)
            {
                b->check_sounds_to_play();
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
            }
        }, &bot)};
        sound_thread.detach();

        Timer bot_save_data_timer;
        bot_save_data_timer.start(1.f / 30.f);

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
            if(bot_save_data_timer.is_time())
            {
                bot.save_data();
                bot_save_data_timer.start(1.f / 30.f);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        }
    }
}

int main(int args, const char** argc)
{

    if(args < 2)
    {
        printf("no broadcaster name found");
        return 0;
    }

    // TODO improve vanish and gamba command

    // TODO
    // periodic messages
    // sounds
    // voices
    // give points
    // social credit system
    // obs websocket api
    // point system
    // cheer notif
    // raid notif

    Bot bot;

    SDL_Init(SDL_INIT_AUDIO);
    Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG);
    Mix_OpenAudio(44000, MIX_DEFAULT_FORMAT, 2, 4096);
    Mix_AllocateChannels(1000000);

    curl_handle = curl_easy_init();

    start_bot(&bot, args, argc);
    
    bot.quit();
    curl_easy_cleanup(curl_handle);
    SDL_Quit();

    return 0;
}
