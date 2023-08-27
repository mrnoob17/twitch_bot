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

inline String base64_decode(const String& b64)
{
    static constexpr const u8 fromBase64[] =
    {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255,  62, 255,  63,
         52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255,   0, 255, 255, 255,
        255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
         15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255,  63,
        255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
         41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255
    };
 

    char* dest {new char [( 3 *  b64.length() / 4 )]};
    char* start {dest};

    for ( u64 i = 0; i < b64.length(); i += 4 )
    {
        u8 byte0 = (b64[i] <= 'z') ? fromBase64[b64[i]] : 0xff;
        u8 byte1 = (b64[i + 1] <= 'z') ? fromBase64[b64[i + 1]] : 0xff;
        u8 byte2 = (b64[i + 2] <= 'z') ? fromBase64[b64[i + 2]] : 0xff;
        u8 byte3 = (b64[i + 3] <= 'z') ? fromBase64[b64[i + 3]] : 0xff;
 
        if (byte1 != 0xff)
            *dest++ = static_cast<u8>(((byte0 & 0x3f ) << 2) + ((byte1 & 0x30) >> 4));
 
        if ( byte2 != 0xff )
            *dest++ = static_cast<u8>(((byte1 & 0x0f ) << 4) + ((byte2 & 0x3c) >> 2));
 
        if ( byte3 != 0xff )
            *dest++ = static_cast<u8>(((byte2 & 0x03 ) << 6) + ((byte3 & 0x3f) >> 0));
    }
    String result {start, start + (dest - start)};
    delete[] start;
    return result;
}



