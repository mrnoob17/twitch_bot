#pragma once
#include "types.hpp"

inline String json_get_value_naive(const String& key, const String& data)
{   
    size_t colon {0};
    size_t quote {0};
    size_t quote_2 {0};

    auto get_valid_colon_helper {[&]()
    {
        colon = data.find(':', colon + 1);
        quote = data.find('"', quote_2 + 1);
        quote_2 = data.find('"', quote + 1);
        while(colon > quote && colon < quote_2)
        {
            colon = data.find(':', quote_2);
            quote = data.find('"', quote_2 + 1);
            quote_2 = data.find('"', quote + 1);
            if(colon == String::npos){
                break;
            }
        }
    }};

    auto get_value {[&]()
    {
        String result;
        while(true)
        {
            get_valid_colon_helper();
            int quote_counter {0};
            if(colon != String::npos)
            {
                auto q {data.rfind('"', colon - 1)};
                auto q2 {data.rfind('"', q - 1)};
                auto k {data.substr(q2 + 1, (q - q2) - 1)};
                if(k == key)
                {
                    q = data.find('"', colon);
                    q2 = data.find('"', q + 1);
                    result = data.substr(q + 1, (q2 - q) - 1);
                    break;
                }
            }
            else {
                break;
            }
        }
        return result;
    }};

    auto result {get_value()};

    return result;
}
