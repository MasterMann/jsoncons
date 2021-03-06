// Copyright 2013 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef JSONCONS_JSONPATH_JSON_QUERY_HPP
#define JSONCONS_JSONPATH_JSON_QUERY_HPP

#include <string>
#include <vector>
#include <memory>
#include <type_traits> // std::is_const
#include <limits> // std::numeric_limits
#include <utility> // std::move
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonpath/jsonpath_filter.hpp>
#include <jsoncons_ext/jsonpath/jsonpath_error.hpp>

namespace jsoncons { namespace jsonpath {

JSONCONS_DEFINE_LITERAL(avg_literal,"avg")
JSONCONS_DEFINE_LITERAL(max_literal,"max")
JSONCONS_DEFINE_LITERAL(min_literal,"min")
JSONCONS_DEFINE_LITERAL(sum_literal,"sum")
JSONCONS_DEFINE_LITERAL(prod_literal,"prod")
JSONCONS_DEFINE_LITERAL(count_literal,"count")

// work around for std::make_unique not being available until C++14
template<typename T, typename... Args>
std::unique_ptr<T> make_unique_ptr(Args&&... args)
{
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

enum class result_type {value,path};

template<class Json>
Json json_query(const Json& root, const typename Json::string_view_type& path, result_type result_t = result_type::value)
{
    std::vector<std::unique_ptr<Json>> temp_json_values;
    if (result_t == result_type::value)
    {
        jsoncons::jsonpath::detail::jsonpath_evaluator<Json,const Json&,detail::VoidPathConstructor<Json>,'$'> evaluator;
        evaluator.evaluate(root, path, temp_json_values);
        return evaluator.get_values();
    }
    else
    {
        jsoncons::jsonpath::detail::jsonpath_evaluator<Json,const Json&,detail::PathConstructor<Json>,'$'> evaluator;
        evaluator.evaluate(root, path, temp_json_values);
        return evaluator.get_normalized_paths();
    }
}

template<class Json, class T>
void json_replace(Json& root, const typename Json::string_view_type& path, T&& new_value)
{
    std::vector<std::unique_ptr<Json>> temp_json_values;
    jsoncons::jsonpath::detail::jsonpath_evaluator<Json,Json&,detail::VoidPathConstructor<Json>,'$'> evaluator;
    evaluator.evaluate(root, path, temp_json_values);
    evaluator.replace(std::forward<T>(new_value));
}

namespace detail {

template<class CharT>
bool try_string_to_index(const CharT *s, size_t length, size_t* value, bool* positive)
{
    static const size_t max_value = (std::numeric_limits<size_t>::max)();
    static const size_t max_value_div_10 = max_value / 10;

    size_t start = 0;
    size_t n = 0;
    if (length > 0)
    {
        if (s[start] == '-')
        {
            *positive = false;
            ++start;
        }
        else
        {
            *positive = true;
        }
    }
    if (length > start)
    {
        for (size_t i = start; i < length; ++i)
        {
            CharT c = s[i];
            switch (c)
            {
            case '0':case '1':case '2':case '3':case '4':case '5':case '6':case '7':case '8':case '9':
                {
                    size_t x = c - '0';
                    if (n > max_value_div_10)
                    {
                        return false;
                    }
                    n = n * 10;
                    if (n > max_value - x)
                    {
                        return false;
                    }

                    n += x;
                }
                break;
            default:
                return false;
                break;
            }
        }
        *value = n;
        return true;
    }
    else
    {
        return false;
    }
}

enum class path_state 
{
    start,
    cr,
    lf,
    expect_dot_or_left_bracket,
    expect_unquoted_name_or_left_bracket,
    unquoted_name,
    left_bracket_single_quoted_string,
    left_bracket_double_quoted_string,
    left_bracket,
    left_bracket_start,
    left_bracket_end,
    left_bracket_end2,
    left_bracket_step,
    left_bracket_step2,
    expect_comma_or_right_bracket,
    function_name,
    function_argument,
    dot
};

template<class Json,
         class JsonReference,
         class PathCons,
         char PathStart>
class jsonpath_evaluator : private serializing_context
{
private:
    typedef typename Json::char_type char_type;
    typedef typename Json::char_traits_type char_traits_type;
    typedef std::basic_string<char_type,char_traits_type> string_type;
    typedef typename Json::string_view_type string_view_type;
    typedef JsonReference reference;
    using pointer = typename std::conditional<std::is_const<typename std::remove_reference<JsonReference>::type>::value,typename Json::const_pointer,typename Json::pointer>::type;

    struct node_type
    {
        node_type() = default;
        node_type(const string_type& p, const pointer& valp)
            : skip_contained_object(false),path(p),val_ptr(valp)
        {
        }
        node_type(string_type&& p, pointer&& valp)
            : skip_contained_object(false),path(std::move(p)),val_ptr(valp)
        {
        }
        node_type(const node_type&) = default;
        node_type(node_type&&) = default;

        bool skip_contained_object;
        string_type path;
        pointer val_ptr;
    };
    typedef std::vector<node_type> node_set;

    static string_view_type length_literal() 
    {
        static const char_type data[] = {'l','e','n','g','t','h'};
        return string_view_type{data,sizeof(data)/sizeof(char_type)};
    }

    class selector
    {
    public:
        virtual ~selector()
        {
        }
        virtual void select(node_type& node, const string_type& path, reference val,
                            node_set& nodes, std::vector<std::unique_ptr<Json>>& temp_json_values) = 0;
    };

    class expr_selector final : public selector
    {
    private:
         jsonpath_filter_expr<Json> result_;
    public:
        expr_selector(const jsonpath_filter_expr<Json>& result)
            : result_(result)
        {
        }

        void select(node_type& node, const string_type& path, reference val, 
                    node_set& nodes, std::vector<std::unique_ptr<Json>>& temp_json_values) override
        {
            auto index = result_.eval(val, temp_json_values);
            if (index.template is<size_t>())
            {
                size_t start = index.template as<size_t>();
                if (val.is_array() && start < val.size())
                {
                    nodes.emplace_back(PathCons()(path,start),std::addressof(val[start]));
                }
            }
            else if (index.is_string())
            {
                name_selector selector(index.as_string_view(),true);
                selector.select(node, path, val, nodes, temp_json_values);
            }
        }
    };

    class filter_selector final : public selector
    {
    private:
         jsonpath_filter_expr<Json> result_;
    public:
        filter_selector(const jsonpath_filter_expr<Json>& result)
            : result_(result)
        {
        }

        void select(node_type& node, const string_type& path, reference val, 
                    node_set& nodes, 
                    std::vector<std::unique_ptr<Json>>& temp_json_values) override
        {
            if (val.is_array())
            {
                node.skip_contained_object =true;
                for (size_t i = 0; i < val.size(); ++i)
                {
                    if (result_.exists(val[i], temp_json_values))
                    {
                        nodes.emplace_back(PathCons()(path,i),std::addressof(val[i]));
                    }
                }
            }
            else if (val.is_object())
            {
                if (!node.skip_contained_object)
                {
                    if (result_.exists(val, temp_json_values))
                    {
                        nodes.emplace_back(path, std::addressof(val));
                    }
                }
                else
                {
                    node.skip_contained_object = false;
                }
            }
        }
    };

    class name_selector final : public selector
    {
    private:
        string_type name_;
        bool positive_start_;
    public:
        name_selector(const string_view_type& name, bool positive_start)
            : name_(name), positive_start_(positive_start)
        {
        }

        void select(node_type&, const string_type& path, reference val,
                    node_set& nodes,
                    std::vector<std::unique_ptr<Json>>& temp_json_values) override
        {
            if (val.is_object() && val.contains(name_))
            {
                nodes.emplace_back(PathCons()(path,name_),std::addressof(val.at(name_)));
            }
            else if (val.is_array())
            {
                size_t pos = 0;
                if (try_string_to_index(name_.data(), name_.size(), &pos, &positive_start_))
                {
                    size_t index = positive_start_ ? pos : val.size() - pos;
                    if (index < val.size())
                    {
                        nodes.emplace_back(PathCons()(path,index),std::addressof(val[index]));
                    }
                }
                else if (name_ == length_literal() && val.size() > 0)
                {
                    auto temp = make_unique_ptr<Json>(val.size());
                    nodes.emplace_back(PathCons()(path, name_), temp.get());
                    temp_json_values.push_back(std::move(temp));
                }
            }
            else if (val.is_string())
            {
                size_t pos = 0;
                string_view_type sv = val.as_string_view();
                if (try_string_to_index(name_.data(), name_.size(), &pos, &positive_start_))
                {
                    size_t index = positive_start_ ? pos : sv.size() - pos;
                    auto sequence = unicons::sequence_at(sv.data(), sv.data() + sv.size(), index);
                    if (sequence.length() > 0)
                    {
                        auto temp = make_unique_ptr<Json>(sequence.begin(),sequence.length());
                        nodes.emplace_back(PathCons()(path, index), temp.get());
                        temp_json_values.push_back(std::move(temp));
                    }
                }
                else if (name_ == length_literal() && sv.size() > 0)
                {
                    size_t count = unicons::u32_length(sv.begin(),sv.end());
                    auto temp = make_unique_ptr<Json>(count);
                    nodes.emplace_back(PathCons()(path,name_),temp.get());
                    temp_json_values.push_back(std::move(temp));
                }
            }
        }
    };

    class array_slice_selector final : public selector
    {
    private:
        size_t start_;
        bool positive_start_;
        size_t end_;
        bool positive_end_;
        bool undefined_end_;
        size_t step_;
        bool positive_step_;
    public:
        array_slice_selector(size_t start, bool positive_start, 
                             size_t end, bool positive_end,
                             size_t step, bool positive_step,
                             bool undefined_end)
            : start_(start), positive_start_(positive_start),
              end_(end), positive_end_(positive_end),undefined_end_(undefined_end),
              step_(step), positive_step_(positive_step) 
        {
        }

        void select(node_type&, const string_type& path, reference val,
                    node_set& nodes,
                    std::vector<std::unique_ptr<Json>>&) override
        {
            if (positive_step_)
            {
                end_array_slice1(path, val, nodes);
            }
            else
            {
                end_array_slice2(path, val, nodes);
            }
        }

        void end_array_slice1(const string_type& path, reference val, node_set& nodes)
        {
            if (val.is_array())
            {
                size_t start = positive_start_ ? start_ : val.size() - start_;
                size_t end;
                if (!undefined_end_)
                {
                    end = positive_end_ ? end_ : val.size() - end_;
                }
                else
                {
                    end = val.size();
                }
                for (size_t j = start; j < end; j += step_)
                {
                    if (j < val.size())
                    {
                        nodes.emplace_back(PathCons()(path,j),std::addressof(val[j]));
                    }
                }
            }
        }

        void end_array_slice2(const string_type& path, reference val, node_set& nodes)
        {
            if (val.is_array())
            {
                size_t start = positive_start_ ? start_ : val.size() - start_;
                size_t end;
                if (!undefined_end_)
                {
                    end = positive_end_ ? end_ : val.size() - end_;
                }
                else
                {
                    end = val.size();
                }

                size_t j = end + step_ - 1;
                while (j > (start+step_-1))
                {
                    j -= step_;
                    if (j < val.size())
                    {
                        nodes.emplace_back(PathCons()(path,j),std::addressof(val[j]));
                    }
                }
            }
        }
    };

    class function_table
    {
        typedef std::function<Json(const std::vector<pointer>&)> function_type;
        typedef std::map<string_type,function_type> function_dictionary;

        const function_dictionary functions_ =
        {
            {
                max_literal<char_type>(),[](const std::vector<pointer>& nodes)
                      {
                          double v = std::numeric_limits<double>::lowest();
                          for (auto& node : nodes)
                          {
                              double x = node->template as<double>();
                              if (x > v)
                              {
                                  v = x;
                              }
                          }
                          return Json(v);
                      }
            },
            {
                min_literal<char_type>(),[](const std::vector<pointer>& nodes) 
                      {
                          std::vector<pointer> result;
                          double v = (std::numeric_limits<double>::max)(); 
                          for (const auto& node : nodes)
                          {
                              double x = node->template as<double>();
                              if (x < v)
                              {
                                  v = x;
                              }
                          }
                          return Json(v);
                      }
            },
            {
                avg_literal<char_type>(),[](const std::vector<pointer>& nodes)
                      {
                          double v = 0.0;
                          for (const auto& node : nodes)
                          {
                              v += node->template as<double>();
                          }
                          return nodes.size() > 0 ? Json(v/nodes.size()) : Json::null();
                      }
            },
            {
                sum_literal<char_type>(),[](const std::vector<pointer>& nodes)
                      {
                          double v = 0.0;
                          for (const auto& node : nodes)
                          {
                              v += node->template as<double>();
                          }
                          return Json(v);
                      }
            },
            {
                count_literal<char_type>(),[](const std::vector<pointer>& nodes)
                      {
                          size_t count = 0;
                          while (count < nodes.size())
                          {
                              ++count;
                          }
                          return Json(count);
                      }
            },
            {
                prod_literal<char_type>(),[](const std::vector<pointer>& nodes)
                      {
                          double v = 0.0;
                          for (const auto& node : nodes)
                          {
                              double x = node->template as<double>();
                              v == 0.0 && x != 0.0
                              ? (v = x)
                              : (v *= x);

                          }
                          return Json(v);
                      }
            }
        };
    public:

        typename function_dictionary::const_iterator find(const string_type& key) const
        {
            return functions_.find(key);
        }
        typename function_dictionary::const_iterator end() const
        {
            return functions_.end();
        }
    };
    function_table functions_;

    default_parse_error_handler default_err_handler_;
    parse_error_handler *err_handler_;
    path_state state_;
    string_type buffer_;
    size_t start_;
    bool positive_start_;
    size_t end_;
    bool positive_end_;
    bool undefined_end_;
    size_t step_;
    bool positive_step_;
    bool recursive_descent_;
    node_set nodes_;
    std::vector<node_set> stack_;
    size_t line_;
    size_t column_;
    const char_type* begin_input_;
    const char_type* end_input_;
    const char_type* p_;
    std::vector<std::unique_ptr<selector>> selectors_;

public:
    jsonpath_evaluator()
        : err_handler_(&default_err_handler_),
          state_(path_state::start),
          start_(0), positive_start_(true), 
          end_(0), positive_end_(true), undefined_end_(false),
          step_(0), positive_step_(true),
          recursive_descent_(false),
          line_(0), column_(0),
          begin_input_(nullptr), end_input_(nullptr),
          p_(nullptr)
    {
    }

    Json get_values() const
    {
        Json result = typename Json::array();

        if (stack_.size() > 0)
        {
            result.reserve(stack_.back().size());
            for (const auto& p : stack_.back())
            {
                result.push_back(*(p.val_ptr));
            }
        }
        return result;
    }

    std::vector<pointer> get_pointers() const
    {
        std::vector<pointer> result;

        if (stack_.size() > 0)
        {
            result.reserve(stack_.back().size());
            for (const auto& p : stack_.back())
            {
                result.push_back(p.val_ptr);
            }
        }
        return result;
    }

    Json get_normalized_paths() const
    {
        Json result = typename Json::array();
        if (stack_.size() > 0)
        {
            result.reserve(stack_.back().size());
            for (const auto& p : stack_.back())
            {
                result.push_back(p.path);
            }
        }
        return result;
    }

    template <class T>
    void replace(T&& new_value)
    {
        if (stack_.size() > 0)
        {
            for (size_t i = 0; i < stack_.back().size(); ++i)
            {
                *(stack_.back()[i].val_ptr) = new_value;
            }
        }
    }

    void evaluate(reference root, const string_view_type& path, std::vector<std::unique_ptr<Json>>& temp_json_values)
    {
        std::error_code ec;
        evaluate(root, path.data(), path.length(), temp_json_values, ec);
        if (ec)
        {
            throw serialization_error(ec, line_, column_);
        }
    }

    void evaluate(reference root, 
                  const string_view_type& path, 
                  std::vector<std::unique_ptr<Json>>& temp_json_values, 
                  std::error_code& ec)
    {
        evaluate(root, path.data(), path.length(), temp_json_values, ec);
    }

    void evaluate(reference root, 
                  const char_type* path, 
                  size_t length,
                  std::vector<std::unique_ptr<Json>>& temp_json_values,
                  std::error_code& ec)
    {
        string_type function_name;
        path_state pre_line_break_state = path_state::start;

        begin_input_ = path;
        end_input_ = path + length;
        p_ = begin_input_;

        line_ = 1;
        column_ = 1;
        state_ = path_state::start;

        recursive_descent_ = false;

        clear_index();

        while (p_ < end_input_)
        {
            switch (state_)
            {
            case path_state::cr:
                ++line_;
                column_ = 1;
                switch (*p_)
                {
                case '\n':
                    state_ = pre_line_break_state;
                    ++p_;
                    ++column_;
                    break;
                default:
                    state_ = pre_line_break_state;
                    break;
                }
                break;
            case path_state::lf:
                ++line_;
                column_ = 1;
                state_ = pre_line_break_state;
                break;
            case path_state::start: 
                switch (*p_)
                {
                    case ' ':case '\t':
                        break;
                    case PathStart:
                    {
                        string_type s;
                        s.push_back(*p_);
                        node_set v;
                        v.emplace_back(std::move(s),std::addressof(root));
                        stack_.push_back(v);

                        state_ = path_state::expect_dot_or_left_bracket;
                        break;
                    }
                    default:
                    {
                        switch (*p_)
                        {
                            case '.':
                            case '[':
                                err_handler_->fatal_error(jsonpath_errc::expected_root, *this);
                                ec = jsonpath_errc::expected_root;
                                return;
                            default: // might be function, validate name later
                                state_ = path_state::function_name;
                                function_name.push_back(*p_);
                                break;
                        }
                        break;
                    }

                    return;
                };
                ++p_;
                ++column_;
                break;
            case path_state::function_name:
                switch (*p_)
                {
                    case '(':
                        state_ = path_state::function_argument;
                        break;
                    default:
                        function_name.push_back(*p_);
                        break;
                }
                ++p_;
                ++column_;
                break;
            case path_state::function_argument:
                switch (*p_)
                {
                case ')':
                {
                    jsonpath_evaluator<Json,JsonReference,PathCons,'$'> evaluator;
                    evaluator.evaluate(root, buffer_, temp_json_values, ec);
                    if (ec)
                    {
                        return;
                    }

                    auto it = functions_.find(function_name);
                    if (it == functions_.end())
                    {
                        ec = jsonpath_errc::invalid_filter_unsupported_operator;
                        return;
                    }
                    auto result = it->second(evaluator.get_pointers());

                    string_type s;
                    s.push_back(PathStart);
                    node_set v;
                    auto temp = make_unique_ptr<Json>(std::move(result));
                    v.emplace_back(std::move(s),temp.get());
                    temp_json_values.push_back(std::move(temp));
                    stack_.push_back(v);

                    state_ = path_state::expect_dot_or_left_bracket;
                    break;
                }
                default:
                    buffer_.push_back(*p_);
                    break;
                }
                ++p_;
                ++column_;
                break;
            case path_state::dot:
                switch (*p_)
                {
                case '.':
                    recursive_descent_ = true;
                    ++p_;
                    ++column_;
                    state_ = path_state::expect_unquoted_name_or_left_bracket;
                    break;
                default:
                    state_ = path_state::expect_unquoted_name_or_left_bracket;
                    break;
                }
                break;
            case path_state::expect_unquoted_name_or_left_bracket:
                switch (*p_)
                {
                case '.':
                    err_handler_->fatal_error(jsonpath_errc::expected_name, *this);
                    ec = jsonpath_errc::expected_name;
                    return;
                case '*':
                    end_all();
                    transfer_nodes();
                    state_ = path_state::expect_dot_or_left_bracket;
                    ++p_;
                    ++column_;
                    break;
                case '[':
                    state_ = path_state::left_bracket;
                    ++p_;
                    ++column_;
                    break;
                default:
                    buffer_.clear();
                    state_ = path_state::unquoted_name;
                    break;
                }
                break;
            case path_state::expect_dot_or_left_bracket: 
                switch (*p_)
                {
                case ' ':case '\t':
                    break;
                case '.':
                    state_ = path_state::dot;
                    break;
                case '[':
                    state_ = path_state::left_bracket;
                    break;
                default:
                    err_handler_->fatal_error(jsonpath_errc::expected_separator, *this);
                    ec = jsonpath_errc::expected_separator;
                    return;
                };
                ++p_;
                ++column_;
                break;
            case path_state::expect_comma_or_right_bracket:
                switch (*p_)
                {
                case ',':
                    state_ = path_state::left_bracket;
                    break;
                case ']':
                    apply_selectors(temp_json_values);
                    state_ = path_state::expect_dot_or_left_bracket;
                    break;
                case ' ':case '\t':
                    break;
                default:
                    err_handler_->fatal_error(jsonpath_errc::expected_right_bracket, *this);
                    ec = jsonpath_errc::expected_right_bracket;
                    return;
                }
                ++p_;
                ++column_;
                break;
            case path_state::left_bracket:
                switch (*p_)
                {
                case ' ':case '\t':
                    ++p_;
                    ++column_;
                    break;
                case '(':
                    {
                        jsonpath_filter_parser<Json> parser(line_,column_);
                        auto result = parser.parse(root, p_,end_input_,&p_, temp_json_values);
                        line_ = parser.line();
                        column_ = parser.column();
                        selectors_.push_back(make_unique_ptr<expr_selector>(result));
                        state_ = path_state::expect_comma_or_right_bracket;
                    }
                    break;
                case '?':
                    {
                        jsonpath_filter_parser<Json> parser(line_,column_);
                        auto result = parser.parse(root,p_,end_input_,&p_, temp_json_values);
                        line_ = parser.line();
                        column_ = parser.column();
                        selectors_.push_back(make_unique_ptr<filter_selector>(result));
                        state_ = path_state::expect_comma_or_right_bracket;
                    }
                    break;                   
                case ':':
                    clear_index();
                    state_ = path_state::left_bracket_end;
                    ++p_;
                    ++column_;
                    break;
                case '*':
                    end_all();
                    state_ = path_state::expect_comma_or_right_bracket;
                    ++p_;
                    ++column_;
                    break;
                case '\'':
                    state_ = path_state::left_bracket_single_quoted_string;
                    ++p_;
                    ++column_;
                    break;
                case '\"':
                    state_ = path_state::left_bracket_double_quoted_string;
                    ++p_;
                    ++column_;
                    break;
                default:
                    clear_index();
                    buffer_.push_back(*p_);
                    state_ = path_state::left_bracket_start;
                    ++p_;
                    ++column_;
                    break;
                }
                break;
            case path_state::left_bracket_start:
                switch (*p_)
                {
                case ':':
                    if (!try_string_to_index(buffer_.data(), buffer_.size(), &start_, &positive_start_))
                    {
                        err_handler_->fatal_error(jsonpath_errc::expected_index, *this);
                        ec = jsonpath_errc::expected_index;
                        return;
                    }
                    state_ = path_state::left_bracket_end;
                    break;
                case ',':
                    selectors_.push_back(make_unique_ptr<name_selector>(buffer_,positive_start_));
                    buffer_.clear();
                    state_ = path_state::left_bracket;
                    break;
                case ']':
                    selectors_.push_back(make_unique_ptr<name_selector>(buffer_,positive_start_));
                    buffer_.clear();
                    apply_selectors(temp_json_values);
                    state_ = path_state::expect_dot_or_left_bracket;
                    break;
                default:
                    buffer_.push_back(*p_);
                    break;
                }
                ++p_;
                ++column_;
                break;
            case path_state::left_bracket_end:
                switch (*p_)
                {
                case '-':
                    positive_end_ = false;
                    state_ = path_state::left_bracket_end2;
                    break;
                case ':':
                    step_ = 0;
                    state_ = path_state::left_bracket_step;
                    break;
                case '0':case '1':case '2':case '3':case '4':case '5':case '6':case '7':case '8':case '9':
                    undefined_end_ = false;
                    end_ = static_cast<size_t>(*p_-'0');
                    state_ = path_state::left_bracket_end2;
                    break;
                case ',':
                    selectors_.push_back(make_unique_ptr<array_slice_selector>(start_,positive_start_,end_,positive_end_,step_,positive_step_,undefined_end_));
                    state_ = path_state::left_bracket;
                    break;
                case ']':
                    selectors_.push_back(make_unique_ptr<array_slice_selector>(start_,positive_start_,end_,positive_end_,step_,positive_step_,undefined_end_));
                    apply_selectors(temp_json_values);
                    state_ = path_state::expect_dot_or_left_bracket;
                    break;
                }
                ++p_;
                ++column_;
                break;
            case path_state::left_bracket_end2:
                switch (*p_)
                {
                case ':':
                    step_ = 0;
                    state_ = path_state::left_bracket_step;
                    break;
                case '0':case '1':case '2':case '3':case '4':case '5':case '6':case '7':case '8':case '9':
                    undefined_end_ = false;
                    end_ = end_*10 + static_cast<size_t>(*p_-'0');
                    break;
                case ',':
                    selectors_.push_back(make_unique_ptr<array_slice_selector>(start_,positive_start_,end_,positive_end_,step_,positive_step_,undefined_end_));
                    state_ = path_state::left_bracket;
                    break;
                case ']':
                    selectors_.push_back(make_unique_ptr<array_slice_selector>(start_,positive_start_,end_,positive_end_,step_,positive_step_,undefined_end_));
                    apply_selectors(temp_json_values);
                    state_ = path_state::expect_dot_or_left_bracket;
                    break;
                }
                ++p_;
                ++column_;
                break;
            case path_state::left_bracket_step:
                switch (*p_)
                {
                case '-':
                    positive_step_ = false;
                    state_ = path_state::left_bracket_step2;
                    break;
                case '0':case '1':case '2':case '3':case '4':case '5':case '6':case '7':case '8':case '9':
                    step_ = static_cast<size_t>(*p_-'0');
                    state_ = path_state::left_bracket_step2;
                    break;
                case ',':
                    selectors_.push_back(make_unique_ptr<array_slice_selector>(start_,positive_start_,end_,positive_end_,step_,positive_step_,undefined_end_));
                    state_ = path_state::left_bracket;
                    break;
                case ']':
                    selectors_.push_back(make_unique_ptr<array_slice_selector>(start_,positive_start_,end_,positive_end_,step_,positive_step_,undefined_end_));
                    apply_selectors(temp_json_values);
                    state_ = path_state::expect_dot_or_left_bracket;
                    break;
                }
                ++p_;
                ++column_;
                break;
            case path_state::left_bracket_step2:
                switch (*p_)
                {
                case '0':case '1':case '2':case '3':case '4':case '5':case '6':case '7':case '8':case '9':
                    step_ = step_*10 + static_cast<size_t>(*p_-'0');
                    break;
                case ',':
                    selectors_.push_back(make_unique_ptr<array_slice_selector>(start_,positive_start_,end_,positive_end_,step_,positive_step_,undefined_end_));
                    state_ = path_state::left_bracket;
                    break;
                case ']':
                    selectors_.push_back(make_unique_ptr<array_slice_selector>(start_,positive_start_,end_,positive_end_,step_,positive_step_,undefined_end_));
                    apply_selectors(temp_json_values);
                    state_ = path_state::expect_dot_or_left_bracket;
                    break;
                }
                ++p_;
                ++column_;
                break;
            case path_state::unquoted_name: 
                switch (*p_)
                {
                case '[':
                    apply_unquoted_string(buffer_, temp_json_values);
                    transfer_nodes();
                    start_ = 0;
                    state_ = path_state::left_bracket;
                    break;
                case '.':
                    apply_unquoted_string(buffer_, temp_json_values);
                    transfer_nodes();
                    state_ = path_state::dot;
                    break;
                case ' ':case '\t':
                    apply_unquoted_string(buffer_, temp_json_values);
                    transfer_nodes();
                    state_ = path_state::expect_dot_or_left_bracket;
                    break;
                case '\r':
                    apply_unquoted_string(buffer_, temp_json_values);
                    transfer_nodes();
                    pre_line_break_state = path_state::expect_dot_or_left_bracket;
                    state_= path_state::cr;
                    break;
                case '\n':
                    apply_unquoted_string(buffer_, temp_json_values);
                    transfer_nodes();
                    pre_line_break_state = path_state::expect_dot_or_left_bracket;
                    state_= path_state::lf;
                    break;
                default:
                    buffer_.push_back(*p_);
                    break;
                };
                ++p_;
                ++column_;
                break;
            case path_state::left_bracket_single_quoted_string: 
                switch (*p_)
                {
                case '\'':
                    selectors_.push_back(make_unique_ptr<name_selector>(buffer_,positive_start_));
                    buffer_.clear();
                    state_ = path_state::expect_comma_or_right_bracket;
                    break;
                case '\\':
                    buffer_.push_back(*p_);
                    if (p_+1 < end_input_)
                    {
                        ++p_;
                        ++column_;
                        buffer_.push_back(*p_);
                    }
                    break;
                default:
                    buffer_.push_back(*p_);
                    break;
                };
                ++p_;
                ++column_;
                break;
            case path_state::left_bracket_double_quoted_string: 
                switch (*p_)
                {
                case '\"':
                    selectors_.push_back(make_unique_ptr<name_selector>(buffer_,positive_start_));
                    buffer_.clear();
                    state_ = path_state::expect_comma_or_right_bracket;
                    break;
                case '\\':
                    buffer_.push_back(*p_);
                    if (p_+1 < end_input_)
                    {
                        ++p_;
                        ++column_;
                        buffer_.push_back(*p_);
                    }
                    break;
                default:
                    buffer_.push_back(*p_);
                    break;
                };
                ++p_;
                ++column_;
                break;
            default:
                ++p_;
                ++column_;
                break;
            }
        }
        switch (state_)
        {
        case path_state::unquoted_name: 
            {
                apply_unquoted_string(buffer_, temp_json_values);
                transfer_nodes();
            }
            break;
        default:
            break;
        }
    }

    void clear_index()
    {
        buffer_.clear();
        start_ = 0;
        positive_start_ = true;
        end_ = 0;
        positive_end_ = true;
        undefined_end_ = true;
        step_ = 1;
        positive_step_ = true;
    }

    void end_all()
    {
        for (size_t i = 0; i < stack_.back().size(); ++i)
        {
            const auto& path = stack_.back()[i].path;
            pointer p = stack_.back()[i].val_ptr;

            if (p->is_array())
            {
                for (auto it = p->array_range().begin(); it != p->array_range().end(); ++it)
                {
                    nodes_.emplace_back(PathCons()(path,it - p->array_range().begin()),std::addressof(*it));
                }
            }
            else if (p->is_object())
            {
                for (auto it = p->object_range().begin(); it != p->object_range().end(); ++it)
                {
                    nodes_.emplace_back(PathCons()(path,it->key()),std::addressof(it->value()));
                }
            }

        }
        start_ = 0;
    }

    void apply_unquoted_string(const string_view_type& name, std::vector<std::unique_ptr<Json>>& temp_json_values)
    {
        if (name.length() > 0)
        {
            for (size_t i = 0; i < stack_.back().size(); ++i)
            {
                apply_unquoted_string(stack_.back()[i].path, *(stack_.back()[i].val_ptr), name, temp_json_values);
            }
        }
        buffer_.clear();
    }

    void apply_unquoted_string(const string_type& path, reference val, const string_view_type& name, std::vector<std::unique_ptr<Json>>& temp_json_values)
    {
        if (val.is_object())
        {
            if (val.contains(name))
            {
                nodes_.emplace_back(PathCons()(path,name),std::addressof(val.at(name)));
            }
            if (recursive_descent_)
            {
                for (auto it = val.object_range().begin(); it != val.object_range().end(); ++it)
                {
                    if (it->value().is_object() || it->value().is_array())
                    {
                        apply_unquoted_string(path, it->value(), name, temp_json_values);
                    }
                }
            }
        }
        else if (val.is_array())
        {
            size_t pos = 0;
            if (try_string_to_index(name.data(),name.size(),&pos, &positive_start_))
            {
                size_t index = positive_start_ ? pos : val.size() - pos;
                if (index < val.size())
                {
                    nodes_.emplace_back(PathCons()(path,index),std::addressof(val[index]));
                }
            }
            else if (name == length_literal() && val.size() > 0)
            {
                auto temp = make_unique_ptr<Json>(val.size());
                nodes_.emplace_back(PathCons()(path,name),temp.get());
                temp_json_values.emplace_back(std::move(temp));
            }
            if (recursive_descent_)
            {
                for (auto it = val.array_range().begin(); it != val.array_range().end(); ++it)
                {
                    if (it->is_object() || it->is_array())
                    {
                        apply_unquoted_string(path, *it, name, temp_json_values);
                    }
                }
            }
        }
        else if (val.is_string())
        {
            string_view_type sv = val.as_string_view();
            size_t pos = 0;
            if (try_string_to_index(name.data(),name.size(),&pos, &positive_start_))
            {
                auto sequence = unicons::sequence_at(sv.data(), sv.data() + sv.size(), pos);
                if (sequence.length() > 0)
                {
                    auto temp = make_unique_ptr<Json>(sequence.begin(),sequence.length());
                    nodes_.emplace_back(PathCons()(path,pos),temp.get());
                    temp_json_values.push_back(std::move(temp));
                }
            }
            else if (name == length_literal() && sv.size() > 0)
            {
                size_t count = unicons::u32_length(sv.begin(),sv.end());
                auto temp = make_unique_ptr<Json>(count);
                nodes_.emplace_back(PathCons()(path,name),temp.get());
                temp_json_values.push_back(std::move(temp));
            }
        }
    }

    void apply_selectors(std::vector<std::unique_ptr<Json>>& temp_json_values)
    {
        if (selectors_.size() > 0)
        {
            for (size_t i = 0; i < stack_.back().size(); ++i)
            {
                node_type& node = stack_.back()[i];
                apply_selectors(node, node.path, *(node.val_ptr), temp_json_values);
            }
            selectors_.clear();
        }
        transfer_nodes();
    }

    void apply_selectors(node_type& node, const string_type& path, reference val,
                         std::vector<std::unique_ptr<Json>>& temp_json_values)
    {
        for (const auto& selector : selectors_)
        {
            selector->select(node, path, val, nodes_, temp_json_values);
        }
        if (recursive_descent_)
        {
            if (val.is_object())
            {
                for (auto& nvp : val.object_range())
                {
                    if (nvp.value().is_object() || nvp.value().is_array())
                    {                        
                        apply_selectors(node,PathCons()(path,nvp.key()),nvp.value(), temp_json_values);
                    }
                }
            }
            else if (val.is_array())
            {
                for (auto& elem : val.array_range())
                {
                    if (elem.is_object() || elem.is_array())
                    {
                        apply_selectors(node,path, elem, temp_json_values);
                    }
                }
            }
        }
    }

    void transfer_nodes()
    {
        stack_.push_back(nodes_);
        nodes_.clear();
        recursive_descent_ = false;
    }

    size_t line_number() const override
    {
        return line_;
    }

    size_t column_number() const override
    {
        return column_;
    }

};

}

}}

#endif
