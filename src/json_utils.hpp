#ifndef JSON_UTILS_HPP
#define JSON_UTILS_HPP

#include <jansson.h>
#include <memory>

struct JSONDeleter { 
    void operator()(json_t* j) const noexcept { 
        if (j) json_decref(j); 
    } 
};

struct JsonStrDeleter { 
    void operator()(char* p) const noexcept { 
        if (p) free(p); 
    } 
};

using JsonPtr = std::unique_ptr<json_t, JSONDeleter>;
using JsonStrPtr = std::unique_ptr<char, JsonStrDeleter>;

#endif
