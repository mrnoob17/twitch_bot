#pragma once

#include <curl/curl.h>
#include "types.hpp"

struct CUrl_Result
{
    size_t size    {0};
    char* response {nullptr};
};

template<typename ...Args>
inline curl_slist* set_curl_headers(const Args&... args)
{
    curl_slist* list {nullptr};

    ((list = curl_slist_append(list, args)), ...);

    return list;
}

inline size_t curl_callback(void* data, size_t size, size_t nmemb, void* clientp)
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
 
inline String curl_call(const String& s, CURL* handle, curl_slist* list = nullptr)
{
    CUrl_Result chunk {0};
    CURLcode res;
    if(handle)
    {
        auto deez {curl_easy_setopt(handle, CURLOPT_URL, s.c_str())};

        deez = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_callback);
     
        deez = curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void *)&chunk);
     
        if(list){
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);
        }

        res = curl_easy_perform(handle);
    }
    String str {chunk.response, chunk.response + chunk.size}; 
    free(chunk.response);
    return str;
}


