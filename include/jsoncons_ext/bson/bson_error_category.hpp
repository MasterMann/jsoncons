/// Copyright 2018 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef JSONCONS_BSON_BSON_ERROR_CATEGORY_HPP
#define JSONCONS_BSON_BSON_ERROR_CATEGORY_HPP

#include <system_error>
#include <jsoncons/config/jsoncons_config.hpp>

namespace jsoncons { namespace bson {

enum class bson_parse_errc
{
    ok = 0,
    unexpected_eof = 1,
    source_error
};

class bson_error_category_impl
   : public std::error_category
{
public:
    virtual const char* name() const noexcept
    {
        return "bson";
    }
    virtual std::string message(int ev) const
    {
        switch (static_cast<bson_parse_errc>(ev))
        {
        case bson_parse_errc::unexpected_eof:
            return "Unexpected end of file";
        case bson_parse_errc::source_error:
            return "Source error";
       default:
            return "Unknown CBOR parser error";
        }
    }
};

inline
const std::error_category& bson_error_category()
{
  static bson_error_category_impl instance;
  return instance;
}

inline 
std::error_code make_error_code(bson_parse_errc result)
{
    return std::error_code(static_cast<int>(result),bson_error_category());
}


}}

namespace std {
    template<>
    struct is_error_code_enum<jsoncons::bson::bson_parse_errc> : public true_type
    {
    };
}

#endif