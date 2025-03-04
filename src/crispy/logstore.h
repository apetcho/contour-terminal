/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <crispy/algorithm.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// NB: Don't do that now. It seems to only cause problems, such as
// __has_include reports presense and in can in fact be included, but it's
// not giving us the expected std::...source_location, wow.
//
// #if __has_include(<source_location>) && !defined(_WIN32)
//     #include <source_location>
// #elif __has_include(<experimental/source_location>)
//     #include <experimental/source_location>
//     #define LOGSTORE_HAS_EXPERIMENTAL_SOURCE_LOCATION 1
// #endif

namespace logstore
{

class Category;
class Sink;

class SourceLocation
{
  public:
    SourceLocation(char const* filename, int line, char const* functionName) noexcept:
        _fileName { filename }, _line { line }, _functionName { functionName }
    {
    }

    [[nodiscard]] char const* file_name() const noexcept { return _fileName; }
    [[nodiscard]] int line() const noexcept { return _line; }
    [[nodiscard]] char const* function_name() const noexcept { return _functionName; }

    static SourceLocation current() noexcept
    {
        return SourceLocation(__builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION());
    }

  private:
    char const* _fileName;
    int _line;
    char const* _functionName;
};

// #if __has_include(<source_location>) && !defined(_WIN32)
// using source_location = std::source_location;
// #elif __has_include(<experimental/source_location>)
// using source_location = std::experimental::source_location;
// #else
using source_location = SourceLocation;
// #endif

class MessageBuilder
{
  private:
    Category const& _category;
    source_location _location;
    std::string _buffer;

  public:
    explicit MessageBuilder(Category const& cat, source_location loc = source_location::current());

    [[nodiscard]] Category const& category() const noexcept { return _category; }
    [[nodiscard]] source_location const& location() const noexcept { return _location; }

    [[nodiscard]] std::string const& text() const noexcept { return _buffer; }

    MessageBuilder& append(std::string_view msg)
    {
        _buffer += msg;
        return *this;
    }

    template <typename... T>
    MessageBuilder& append(fmt::format_string<T...> fmt, T&&... args)
    {
        _buffer += fmt::vformat(fmt, fmt::make_format_args(args...));
        return *this;
    }

    MessageBuilder& operator()(std::string const& msg)
    {
        _buffer += msg;
        return *this;
    }
    template <typename... T>
    MessageBuilder& operator()(fmt::format_string<T...> fmt, T&&... args)
    {
        _buffer += fmt::vformat(fmt, fmt::make_format_args(args...));
        return *this;
    }

    [[nodiscard]] std::string message() const;

    ~MessageBuilder();
};

/// Defines a logging Category, such as: error, warning, metrics, vt.backend, or renderer.
///
/// A program can have multiple logging categories, all pointing to the same
/// or each to an individual logging sink.
class Category
{
  public:
    using Formatter = std::function<std::string(MessageBuilder const&)>;
    enum class State
    {
        Enabled,
        Disabled
    };
    enum class Visibility
    {
        Public,
        Hidden
    };

    Category(std::string_view name,
             std::string_view desc,
             State state = State::Disabled,
             Visibility visibility = Visibility::Public) noexcept;
    ~Category();

    [[nodiscard]] std::string_view name() const noexcept { return _name; }
    [[nodiscard]] std::string_view description() const noexcept { return _description; }

    [[nodiscard]] bool is_enabled() const noexcept { return _state == State::Enabled; }
    void enable(bool enabled = true) noexcept { _state = enabled ? State::Enabled : State::Disabled; }
    void disable() noexcept { _state = State::Disabled; }

    [[nodiscard]] bool visible() const noexcept { return _visibility == Visibility::Public; }
    void set_visible(bool visible) { _visibility = visible ? Visibility::Public : Visibility::Hidden; }

    operator bool() const noexcept { return is_enabled(); }

    [[nodiscard]] Formatter const& formatter() const { return _formatter; }
    void set_formatter(Formatter formatter) { _formatter = std::move(formatter); }

    void set_sink(logstore::Sink& s) { _sink = s; }
    [[nodiscard]] logstore::Sink& sink() const noexcept { return _sink.get(); }

    [[nodiscard]] MessageBuilder build(source_location location = source_location::current()) const
    {
        return MessageBuilder(*this, location);
    }

    [[nodiscard]] MessageBuilder operator()(source_location location = source_location::current()) const
    {
        return MessageBuilder(*this, location);
    }

    static std::string defaultFormatter(MessageBuilder const& message);

  private:
    std::string_view _name;
    std::string_view _description;
    State _state;
    Visibility _visibility;
    Formatter _formatter;
    std::reference_wrapper<logstore::Sink> _sink;
};

/// Logging Sink API.
///
/// Such as the console, a log file, or UDP endpoint.
class Sink
{
  public:
    using Writer = std::function<void(std::string_view const&)>;

    Sink(bool enabled, Writer writer);
    Sink(bool enabled, std::ostream& output);
    Sink(bool enabled, std::shared_ptr<std::ostream> f);

    void set_writer(Writer writer);

    /// Writes given built message to this sink.
    void write(MessageBuilder const& message);

    void set_enabled(bool enabled) { _enabled = enabled; }

    /// Retrieves reference to standard debug-logging sink.
    static Sink& console();
    static Sink& error_console(); // NOLINT(readability-identifier-naming)

  private:
    bool _enabled;
    Writer _writer;
};

std::vector<std::reference_wrapper<Category>>& get();
Category* get(std::string_view categoryName);
void set_sink(Sink& sink);
void set_formatter(Category::Formatter const& f);
void enable(std::string_view categoryName, bool enabled = true);
void disable(std::string_view categoryName);
void configure(std::string_view filterString);

// {{{ implementation
inline std::string MessageBuilder::message() const
{
    if (_category.formatter())
        return _category.formatter()(*this);
    else if (!_buffer.empty() && _buffer.back() == '\n')
        return _buffer;
    else if (!_buffer.empty())
        return _buffer + '\n';
    else
        return "";
}

inline std::vector<std::reference_wrapper<Category>>& get()
{
    static std::vector<std::reference_wrapper<Category>> logStore;
    return logStore;
}

inline Category* get(std::string_view categoryName)
{
    for (auto const& cat: get())
        if (cat.get().name() == categoryName)
            return &cat.get();
    return nullptr;
}

inline void set_sink(Sink& s)
{
    for (auto const& cat: get())
        cat.get().set_sink(s);
}

inline void set_formatter(Category::Formatter const& f)
{
    for (auto const& cat: get())
        cat.get().set_formatter(f);
}

inline void enable(std::string_view categoryName, bool enabled)
{
    for (auto const& cat: get())
        if (cat.get().name() == categoryName)
            cat.get().enable(enabled);
}

inline void disable(std::string_view categoryName)
{
    enable(categoryName, false);
}

inline void configure(std::string_view filterString)
{
    if (filterString == "all")
    {
        for (auto& category: logstore::get())
            category.get().enable();
    }
    else
    {
        auto const filters = crispy::split(filterString, ',');
        for (auto& category: logstore::get())
        {
            category.get().enable(crispy::any_of(filters, [&](std::string_view filterPattern) -> bool {
                if (filterPattern.back() != '*')
                    return category.get().name() == filterPattern;
                // TODO: '*' excludes hidden categories
                return std::equal(std::begin(filterPattern),
                                  std::prev(end(filterPattern)),
                                  std::begin(category.get().name()));
            }));
        }
    }
}

inline MessageBuilder::MessageBuilder(logstore::Category const& cat, source_location location):
    _category { cat }, _location { location }
{
}

inline MessageBuilder::~MessageBuilder()
{
    _category.sink().write(*this);
}

inline Category::Category(std::string_view name,
                          std::string_view desc,
                          State state,
                          Visibility visibility) noexcept:
    _name { name },
    _description { desc },
    _state { state },
    _visibility { visibility },
    _sink { logstore::Sink::console() }
{
    assert(std::none_of(get().begin(), get().end(), [&](Category const& x) { return x.name() == _name; }));
    get().emplace_back(*this);
}

inline Category::~Category()
{
    for (auto i = get().begin(), e = get().end(); i != e; ++i)
    {
        if (&i->get() == this)
        {
            get().erase(i);
            break;
        }
    }
}

inline std::string Category::defaultFormatter(MessageBuilder const& message)
{
    return fmt::format("[{}:{}:{}]: {}\n",
                       message.category().name(),
                       message.location().file_name(),
                       message.location().line(),
                       message.text());
}

inline void Sink::write(MessageBuilder const& message)
{
    if (_enabled && message.category().is_enabled())
        _writer(message.message());
}

inline void Sink::set_writer(Writer writer)
{
    _writer = std::move(writer);
}
// }}}

auto inline ErrorLog = logstore::Category("error", "Error Logger", Category::State::Enabled);

#define errorlog() (::logstore::ErrorLog())

} // namespace logstore
