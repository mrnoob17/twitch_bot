#pragma once
#include "types.hpp"
#include <sstream>

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
    void start()
    {
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

inline void clean_line(String* str)
{
    if(str->empty()){
        return;
    }
    auto is_white_space {[](const char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n'; 
    }};

    while(is_white_space(str->front()))
    {
        str->erase(0, 1); 
        if(str->empty()){
            return;
        }
    }

    while(is_white_space(str->back()))
    {
        str->pop_back();
        if(str->empty()){
            return;
        }
    }
}

inline void string_capitalize(String* s)
{
    for(auto& c : *s){
        c = toupper(c);
    }
}

inline void string_decapitalize(String* s)
{
    for(auto& c : *s){
        c = tolower(c);
    }
}

inline void extract_tag_and_value_from_line(String s, String* tag, String* value, const char sep = ':')
{
    if(s.empty())
    {
        *tag = {};
        *value = {};
        return;
    }

    auto d {s.find(sep)};

    *tag = s.substr(0, d);
    *value = s.substr(d + 1);
    clean_line(tag);
    clean_line(value);
}

inline Vector<String> tokenize(const String& str)
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

inline String wrap_in_quotes(const String& s)
{
    return '"' + (s) + '"';
}

inline int string_to_int(const String& s)
{
    std::stringstream ss {s};
    int res;
    ss>>res;
    return res;
}

inline float string_to_float(const String& s)
{
    std::stringstream ss {s};
    float res;
    ss>>res;
    return res;
}
