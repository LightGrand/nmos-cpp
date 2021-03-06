#ifndef CPPREST_JSON_UTILS_H
#define CPPREST_JSON_UTILS_H

#include "cpprest/json.h"
#include "cpprest/base_uri.h" // for web::uri::decode

// since some of the constructors are explicit, provide helpers
#ifndef _TURN_OFF_PLATFORM_STRING
#define J(x) web::json::value{x}
#define JU(x) J(U(x))
#endif

// no idea why these operators aren't provided?
namespace web
{
    namespace json
    {
        inline bool operator==(const web::json::object& lhs, const web::json::object& rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            using std::begin;
            using std::end;
            // equality respects keep_order 
            return std::equal(begin(lhs), end(lhs), begin(rhs));
        }

        inline bool operator!=(const web::json::object& lhs, const web::json::object& rhs)
        {
            return !(lhs == rhs);
        }

        inline bool operator==(const web::json::array& lhs, const web::json::array& rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            using std::begin;
            using std::end;
            return std::equal(begin(lhs), end(lhs), begin(rhs));
        }

        inline bool operator!=(const web::json::array& lhs, const web::json::array& rhs)
        {
            return !(lhs == rhs);
        }
    }
}

// json field accessor helpers
namespace web
{
    namespace json
    {
        template <typename T> T as(const web::json::value& value) { return{ value }; }
        template <> inline web::json::value as<web::json::value>(const web::json::value& value) { return value; }
        template <> inline web::json::object as<web::json::object>(const web::json::value& value) { return value.as_object(); }
        template <> inline web::json::array as<web::json::array>(const web::json::value& value) { return value.as_array(); }
        template <> inline utility::string_t as<utility::string_t>(const web::json::value& value) { return value.as_string(); }
        template <> inline bool as<bool>(const web::json::value& value) { return value.as_bool(); }
        template <> inline int as<int>(const web::json::value& value) { return value.as_integer(); }

        template <typename T> struct field
        {
            utility::string_t key;
            operator const utility::string_t&() const { return key; }
            T operator()(const web::json::value& value) const { return as<T>(value.at(key)); }
        };

        typedef field<web::json::value> field_as_value;
        typedef field<web::json::object> field_as_object;
        typedef field<web::json::array> field_as_array;
        typedef field<utility::string_t> field_as_string;
        typedef field<bool> field_as_bool;
        typedef field<int> field_as_integer;

        template <typename T> struct field_with_default
        {
            utility::string_t key;
            T or;
            operator const utility::string_t&() const { return key; }
            T operator()(const web::json::value& value) const
            {
                web::json::object::const_iterator it;
                return value.is_object() && value.as_object().end() != (it = value.as_object().find(key)) ? as<T>(it->second) : or;
            }
        };

        typedef field_with_default<utility::string_t> field_as_string_or;
        typedef field_with_default<bool> field_as_bool_or;
        typedef field_with_default<int> field_as_integer_or;
    }
}

// web::json::value has a rather limited interface for the construction of objects and arrays;
// value_from_fields is a helper function similar to the web::json::value::object factory function
// but which can handle e.g. std::map<utility:string_t, utility:string_t>
// value_from_elements is similarly related to the web::json::value::array factory function
// value_of allows construction of both objects and array from a braced-init-list
namespace web
{
    namespace json
    {
        template <typename KeyValuePair>
        inline void insert(web::json::value& value, const KeyValuePair& field)
        {
            value[field.first] = web::json::value{ field.second };
        }

        template <typename KeyValuePairs>
        inline web::json::value value_from_fields(const KeyValuePairs& fields, bool keep_order = false)
        {
            web::json::value result = web::json::value::object(keep_order);
            for (auto& field : fields)
            {
                insert(result, field);
            }
            return result;
        }

        template <typename Value>
        inline void push_back(web::json::value& value, const Value& element)
        {
            value[value.size()] = web::json::value{ element };
        }

        template <typename Values>
        inline web::json::value value_from_elements(const Values& elements)
        {
            web::json::value result = web::json::value::array();
            for (auto& element : elements)
            {
                push_back(result, element);
            }
            return result;
        }

        inline web::json::value value_of(std::initializer_list<std::pair<utility::string_t, value>> fields, bool keep_order = false)
        {
            web::json::value result = web::json::value::object(keep_order);
            for (auto& field : fields)
            {
                insert(result, field);
            }
            return result;
        }

        inline web::json::value value_of(std::initializer_list<value> elements)
        {
            web::json::value result = web::json::value::array();
            for (auto& element : elements)
            {
                push_back(result, element);
            }
            return result;
        }

        inline web::json::value& front(web::json::value& value)
        {
            return value.at(0);
        }

        inline const web::json::value& front(const web::json::value& value)
        {
            return value.at(0);
        }

        inline web::json::value& back(web::json::value& value)
        {
            return value.at(value.size() - 1);
        }

        inline const web::json::value& back(const web::json::value& value)
        {
            return value.at(value.size() - 1);
        }
    }
}

// json serialization helpers
namespace web
{
    namespace json
    {
        // filter, transform and serialize a forward range (e.g. vector, list) of json values as an array
        // (yes, this could be factored into independent range filter, transform and serialize operations, but bah!)
        template <typename ForwardRange, typename Pred, typename Transform>
        inline void serialize_if(utility::ostream_t& os, const ForwardRange& range, Pred pred, Transform transform)
        {
            os << U('[');
            bool empty = true;
            for (auto& element : range)
            {
                if (pred(element))
                {
                    if (!empty)
                    {
                        os << U(',');
                    }
                    else
                    {
                        empty = false;
                    }
                    transform(element).serialize(os);
                }
            }
            os << U(']');
        }

        template <typename ForwardRange, typename Pred, typename Transform>
        inline utility::string_t serialize_if(const ForwardRange& range, Pred pred, Transform transform)
        {
            utility::ostringstream_t os;
            serialize_if(os, range, pred, transform);
            return os.str();
        }

        template <typename ForwardRange, typename Pred>
        inline utility::string_t serialize_if(const ForwardRange& range, Pred pred)
        {
            utility::ostringstream_t os;
            using std::begin;
            serialize_if(os, range, pred, [](const typename std::iterator_traits<decltype(begin(range))>::value_type& element) { return element; });
            return os.str();
        }

        template <typename ForwardRange, typename Transform>
        inline utility::string_t serialize(const ForwardRange& range, Transform transform)
        {
            utility::ostringstream_t os;
            using std::begin;
            serialize_if(os, range, [](const typename std::iterator_traits<decltype(begin(range))>::value_type& element) { return true; }, transform);
            return os.str();
        }

        template <typename ForwardRange>
        inline utility::string_t serialize(const ForwardRange& range)
        {
            utility::ostringstream_t os;
            using std::begin;
            serialize_if(os, range, [](const typename std::iterator_traits<decltype(begin(range))>::value_type& element) { return true; });
            return os.str();
        }
    }
}

// json query helpers
namespace web
{
    namespace json
    {
        // insert a value into an object, splitting the key path on '.' and inserting sub-objects as necessary
        void insert(web::json::object& object, const utility::string_t key_path, const web::json::value& value);

        // find a value or values from an object, splitting the key path on '.' and searching arrays as necessary
        // returns true if the object matches the key path
        // if any arrays are encountered on the key path, results is an array, otherwise it's a non-array value
        bool extract(const web::json::object& object, web::json::value& results, const utility::string_t& key_path);

        // match_flag_type is a bitmask
        enum match_flag_type
        {
            match_default = 0x0000, // string matches must be exact
            match_substr =  0x0001, // substring matches are OK
            match_icase =   0x0002  // case-insensitive matches are OK
        };
        // so for some convenience...
        inline match_flag_type operator|(match_flag_type lhs, match_flag_type rhs) { return match_flag_type((int)lhs | (int)rhs); }

        // construct an ordered parameters object from a URI-encoded query string of '&' or ';' separated terms expected to be field=value pairs
        // field names will be URI-decoded, but values will be left as-is!
        // cf. web::uri::split_query
        web::json::value value_from_query(const utility::string_t& query);

        // construct a query/exemplar object from a parameters object, by constructing nested sub-objects from '.'-separated field names
        web::json::value unflatten(const web::json::value& value);

        // compare a value against a query/exemplar
        bool match_query(const web::json::value& value, const web::json::value& query, match_flag_type match_flags = match_default);
    }
}

#endif
