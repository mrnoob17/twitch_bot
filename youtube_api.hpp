#pragma once

#include "types.hpp"
#include <sstream>
#include "utilities.hpp"

struct Youtube_Video_Info
{
    String title    {};
    String iso_8601_duration;
    int like_count  {-1};
    int view_count  {-1};
    size_t duration {0};
};

Youtube_Video_Info parse_youtube_api_result(const String& s)
{
    String line;
    std::stringstream ss {s};

    auto get_content {[](const String& l)
    {
        auto colon {l.find(':')};
        auto first_comma {l.find('"', colon + 1)};
        auto second_comma {l.find('"', first_comma + 1)};
        return l.substr(first_comma + 1, (second_comma - first_comma) - 1);
    }};

    auto convert_iso_8601_to_seconds {[](const String& s)
    {
        auto get_number_component {[&s](size_t ctr)
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
            else if(line.find("likeCount") != String::npos){
                result.like_count = string_to_int(get_content(line));
            }
        }
    }
    return result;
};
