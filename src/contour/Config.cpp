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
#include "Config.h"

#include <vtbackend/ControlCode.h>
#include <vtbackend/InputGenerator.h>

#include <vtpty/Process.h>

#include <text_shaper/mock_font_locator.h>

#include <crispy/escape.h>
#include <crispy/logstore.h>
#include <crispy/overloaded.h>
#include <crispy/stdfs.h>
#include <crispy/utils.h>

#include <yaml-cpp/node/detail/iterator_fwd.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <yaml-cpp/yaml.h>

#include <QtCore/QFile>
#include <QtGui/QOpenGLContext>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "contour/Actions.h"
#include "vtbackend/ColorPalette.h"
#include "vtbackend/primitives.h"

#if defined(_WIN32)
    #include <Windows.h>
#elif defined(__APPLE__)
    #include <unistd.h>

    #include <mach-o/dyld.h>
#else
    #include <unistd.h>
#endif

auto constexpr MinimumFontSize = text::font_size { 8.0 };

using namespace std;
using crispy::escape;
using crispy::homeResolvedPath;
using crispy::replaceVariables;
using crispy::toLower;
using crispy::toUpper;
using crispy::unescape;

using terminal::Height;
using terminal::ImageSize;
using terminal::Process;
using terminal::Width;

using terminal::CellRGBColorAndAlphaPair;
using terminal::ColumnCount;
using terminal::Infinite;
using terminal::LineCount;
using terminal::PageSize;

using contour::actions::Action;

using UsedKeys = set<string>;

namespace contour::config
{

namespace
{
    auto const ConfigLog = logstore::Category("config", "Logs configuration file loading.");

    string processIdAsString()
    {
        // There's sadly no better way to platfrom-independantly get the PID.
        auto stringStream = std::stringstream();
#if defined(_WIN32)
        stringStream << static_cast<unsigned>(GetCurrentProcessId());
#else
        stringStream << getpid();
#endif
        return stringStream.str();
    }

    struct VariableReplacer
    {
        auto operator()(string_view name) -> string
        {
            if (name == "pid")
                return processIdAsString();
            return ""s;
        }
    };

    std::shared_ptr<terminal::BackgroundImage const> loadImage(string const& fileName,
                                                               float opacity,
                                                               bool blur)
    {
        auto const resolvedFileName = homeResolvedPath(fileName, Process::homeDirectory());

        if (!FileSystem::exists(resolvedFileName))
        {
            errorlog()("Background image path not found: {}", resolvedFileName.string());
            return nullptr;
        }

        auto backgroundImage = terminal::BackgroundImage {};
        backgroundImage.location = resolvedFileName;
        backgroundImage.hash = crispy::StrongHash::compute(resolvedFileName.string());
        backgroundImage.opacity = opacity;
        backgroundImage.blur = blur;

        return make_shared<terminal::BackgroundImage const>(std::move(backgroundImage));
    }

    terminal::CellRGBColor parseCellColor(std::string const& _text)
    {
        auto const text = toUpper(_text);
        if (text == "CELLBACKGROUND"sv)
            return terminal::CellBackgroundColor {};
        if (text == "CELLFOREGROUND"sv)
            return terminal::CellForegroundColor {};
        return terminal::RGBColor(_text);
    }

    terminal::CellRGBColor parseCellColor(UsedKeys& usedKeys,
                                          YAML::Node const& parentNode,
                                          std::string const& parentPath,
                                          std::string const& name,
                                          terminal::CellRGBColor defaultValue)
    {
        auto colorNode = parentNode[name];
        if (!colorNode || !colorNode.IsScalar())
            return defaultValue;
        usedKeys.emplace(parentPath + "." + name);
        return parseCellColor(colorNode.as<string>());
    }

    std::optional<terminal::RGBColorPair> parseRGBColorPair(UsedKeys& usedKeys,
                                                            string const& basePath,
                                                            YAML::Node const& baseNode,
                                                            string const& childNodeName,
                                                            terminal::RGBColorPair defaultPair)
    {
        auto node = baseNode[childNodeName];
        if (!node || !node.IsMap())
            return nullopt;

        auto const childPath = fmt::format("{}.{}", basePath, childNodeName);
        usedKeys.emplace(childPath);

        auto rgbColorPair = defaultPair;

        if (auto const value = node["foreground"]; value && value.IsScalar())
        {
            rgbColorPair.foreground = value.as<string>();
            usedKeys.emplace(childPath + ".foreground");
        }

        if (auto const value = node["background"]; value && value.IsScalar())
        {
            rgbColorPair.background = value.as<string>();
            usedKeys.emplace(childPath + ".background");
        }

        return rgbColorPair;
    }

    /// Loads a configuration sub-section to handle cell color foreground/background + alpha.
    ///
    /// Example:
    ///   { foreground: CellColor, foreground_alpha: FLOAT = 1.0,
    ///     background: CellColor, background_alpha: FLOAT = 1.0 }
    std::optional<CellRGBColorAndAlphaPair> parseCellRGBColorAndAlphaPair(UsedKeys& usedKeys,
                                                                          string const& basePath,
                                                                          YAML::Node const& baseNode,
                                                                          string const& childNodeName)
    {
        auto node = baseNode[childNodeName];
        if (!node)
            return nullopt;

        auto const childPath = fmt::format("{}.{}", basePath, childNodeName);
        usedKeys.emplace(childPath);

        auto cellRGBColorAndAlphaPair = CellRGBColorAndAlphaPair {};

        cellRGBColorAndAlphaPair.foreground =
            parseCellColor(usedKeys, node, childPath, "foreground", terminal::CellForegroundColor {});
        if (auto alpha = node["foreground_alpha"]; alpha && alpha.IsScalar())
        {
            usedKeys.emplace(childPath + ".foreground_alpha");
            cellRGBColorAndAlphaPair.foregroundAlpha = std::clamp(alpha.as<float>(), 0.0f, 1.0f);
        }

        cellRGBColorAndAlphaPair.background =
            parseCellColor(usedKeys, node, childPath, "background", terminal::CellBackgroundColor {});
        if (auto alpha = node["background_alpha"]; alpha && alpha.IsScalar())
        {
            usedKeys.emplace(childPath + ".background_alpha");
            cellRGBColorAndAlphaPair.backgroundAlpha = std::clamp(alpha.as<float>(), 0.0f, 1.0f);
        }

        return cellRGBColorAndAlphaPair;
    }

    // TODO:
    // - [x] report missing keys
    // - [ ] report superfluous keys (by keeping track of loaded keys, then iterate
    //       through full document and report any key that has not been loaded but is available)
    // - [ ] Do we want to report when no color schemes are defined? (at least warn about?)
    // - [ ] Do we want to report when no input mappings are defined? (at least warn about?)

    vector<FileSystem::path> getTermInfoDirs(optional<FileSystem::path> const& _appTerminfoDir)
    {
        auto locations = vector<FileSystem::path>();

        if (_appTerminfoDir.has_value())
            locations.emplace_back(_appTerminfoDir.value().string());

        locations.emplace_back(getenv("HOME") + "/.terminfo"s);

        if (auto const* value = getenv("TERMINFO_DIRS"); value && *value)
            for (auto const dir: crispy::split(string_view(value), ':'))
                locations.emplace_back(string(dir));

        locations.emplace_back("/usr/share/terminfo");

        return locations;
    }

    string getDefaultTERM(optional<FileSystem::path> const& _appTerminfoDir)
    {
#if defined(_WIN32)
        return "contour";
#else

        if (Process::isFlatpak())
            return "contour";

        auto locations = getTermInfoDirs(_appTerminfoDir);
        auto const terms = vector<string> {
            "contour", "contour-latest", "xterm-256color", "xterm", "vt340", "vt220",
        };

        for (auto const& prefix: locations)
            for (auto const& term: terms)
            {
                if (access((prefix / term.substr(0, 1) / term).string().c_str(), R_OK) == 0)
                    return term;

    #if defined(__APPLE__)
                // I realized that on Apple the `tic` command sometimes installs
                // the terminfo files into weird paths.
                if (access((prefix / fmt::format("{:02X}", term.at(0)) / term).string().c_str(), R_OK) == 0)
                    return term;
    #endif
            }

        return "vt100";
#endif
    }

    optional<Permission> toPermission(string const& _value)
    {
        if (_value == "allow")
            return Permission::Allow;
        else if (_value == "deny")
            return Permission::Deny;
        else if (_value == "ask")
            return Permission::Ask;
        return nullopt;
    }

    void createFileIfNotExists(FileSystem::path const& _path)
    {
        if (!FileSystem::is_regular_file(_path))
            if (auto const ec = createDefaultConfig(_path); ec)
                throw runtime_error { fmt::format(
                    "Could not create directory {}. {}", _path.parent_path().string(), ec.message()) };
    }

    template <typename T>
    bool tryLoadValueRelative(UsedKeys& _usedKeys,
                              YAML::Node const& _currentNode,
                              string const& _basePath,
                              vector<string_view> const& _keys,
                              size_t _offset,
                              T& _store,
                              logstore::MessageBuilder const& _logger)
    {
        string parentKey = _basePath;
        for (size_t i = 0; i < _offset; ++i)
        {
            if (!parentKey.empty())
                parentKey += '.';
            parentKey += _keys.at(i);
        }

        if (_offset == _keys.size())
        {
            _store = _currentNode.as<T>();
            return true;
        }

        auto const currentKey = string(_keys.at(_offset));

        auto const child = _currentNode[currentKey];
        if (!child)
        {
            auto const defaultStr = crispy::escape(fmt::format("{}", _store));
            auto const defaultStrQuoted = !defaultStr.empty() ? defaultStr : R"("")";
            for (size_t i = _offset; i < _keys.size(); ++i)
            {
                if (!parentKey.empty())
                    parentKey += '.';
                parentKey += _keys[i];
            }
            _logger("Missing key {}. Using default: {}.", parentKey, defaultStrQuoted);
            return false;
        }

        _usedKeys.emplace(parentKey);

        return tryLoadValueRelative(_usedKeys, child, _keys, _offset + 1, _store, _logger);
    }

    template <typename T>
    bool tryLoadValue(UsedKeys& _usedKeys,
                      YAML::Node const& _root,
                      vector<string_view> const& _keys,
                      size_t _offset,
                      T& _store,
                      logstore::MessageBuilder _logger)
    {
        string parentKey;
        for (size_t i = 0; i < _offset; ++i)
        {
            if (i)
                parentKey += '.';
            parentKey += _keys.at(i);
        }

        if (_offset == _keys.size())
        {
            _store = _root.as<T>();
            return true;
        }

        auto const currentKey = string(_keys.at(_offset));

        auto const child = _root[currentKey];
        if (!child)
        {
            auto const defaultStr = crispy::escape(fmt::format("{}", _store));
            for (size_t i = _offset; i < _keys.size(); ++i)
            {
                parentKey += '.';
                parentKey += _keys[i];
            }
            _logger(
                "Missing key {}. Using default: {}.", parentKey, !defaultStr.empty() ? defaultStr : R"("")");
            return false;
        }

        _usedKeys.emplace(parentKey);

        return tryLoadValue(_usedKeys, child, _keys, _offset + 1, _store, _logger);
    }

    template <typename T, typename U>
    bool tryLoadValue(UsedKeys& _usedKeys,
                      YAML::Node const& _root,
                      vector<string_view> const& _keys,
                      size_t _offset,
                      crispy::boxed<T, U>& _store,
                      logstore::MessageBuilder const& _logger)
    {
        return tryLoadValue(_usedKeys, _root, _keys, _offset, _store.value, _logger);
    }

    template <typename T>
    bool tryLoadValue(UsedKeys& _usedKeys,
                      YAML::Node const& _root,
                      string const& _path,
                      T& _store,
                      logstore::MessageBuilder const& _logger)
    {
        auto const keys = crispy::split(_path, '.');
        _usedKeys.emplace(_path);
        return tryLoadValue(_usedKeys, _root, keys, 0, _store, _logger);
    }

    template <typename T, typename U>
    bool tryLoadValue(UsedKeys& _usedKeys,
                      YAML::Node const& _root,
                      string const& _path,
                      crispy::boxed<T, U>& _store,
                      logstore::MessageBuilder const& _logger)
    {
        return tryLoadValue(_usedKeys, _root, _path, _store.value, _logger);
    }

    template <typename T>
    bool tryLoadChild(UsedKeys& _usedKeys,
                      YAML::Node const& _doc,
                      string const& _parentPath,
                      string const& _key,
                      T& _store,
                      logstore::MessageBuilder const& _logger)
    {
        auto const path = fmt::format("{}.{}", _parentPath, _key);
        return tryLoadValue(_usedKeys, _doc, path, _store, _logger);
    }

    template <typename T>
    bool tryLoadChildRelative(UsedKeys& _usedKeys,
                              YAML::Node const& _node,
                              string const& _parentPath,
                              string const& _childKeyPath,
                              T& _store,
                              logstore::MessageBuilder const& _logger)
    {
        // return tryLoadValue(_usedKeys, _node, _childKeyPath, _store); // XXX _parentPath
        auto const keys = crispy::split(_childKeyPath, '.');
        string s = _parentPath;
        for (auto const key: keys)
        {
            s += fmt::format(".{}", key);
            _usedKeys.emplace(s);
        }
        return tryLoadValue(_usedKeys, _node, keys, 0, _store, _logger);
    }

    template <typename T, typename U>
    bool tryLoadChild(UsedKeys& _usedKeys,
                      YAML::Node const& _doc,
                      string const& _parentPath,
                      string const& _key,
                      crispy::boxed<T, U>& _store,
                      logstore::MessageBuilder const& _logger)
    {
        return tryLoadChild(_usedKeys, _doc, _parentPath, _key, _store.value, _logger);
    }

    void checkForSuperfluousKeys(YAML::Node _root, string const& _prefix, UsedKeys const& _usedKeys)
    {
        if (_root.IsMap())
        {
            for (auto const& mapItem: _root)
            {
                auto const name = mapItem.first.as<string>();
                auto const child = mapItem.second;
                auto const prefix = _prefix.empty() ? name : fmt::format("{}.{}", _prefix, name);
                checkForSuperfluousKeys(child, prefix, _usedKeys);
                if (_usedKeys.count(prefix))
                    continue;
                if (crispy::startsWith(string_view(prefix), "x-"sv))
                    continue;
                errorlog()("Superfluous config key found: {}", escape(prefix));
            }
        }
        else if (_root.IsSequence())
        {
            for (size_t i = 0; i < _root.size() && i < 8; ++i)
            {
                checkForSuperfluousKeys(_root[i], fmt::format("{}.{}", _prefix, i), _usedKeys);
            }
        }
#if 0
        else if (_root.IsScalar())
        {
        }
        else if (_root.IsNull())
        {
            ; // no-op
        }
#endif
    }

    void checkForSuperfluousKeys(YAML::Node const& _root, UsedKeys const& _usedKeys)
    {
        checkForSuperfluousKeys(_root, "", _usedKeys);
    }

    optional<std::string> readFile(FileSystem::path const& _path)
    {
        if (!FileSystem::exists(_path))
            return nullopt;

        auto ifs = ifstream(_path.string());
        if (!ifs.good())
            return nullopt;

        auto const size = FileSystem::file_size(_path);
        auto text = string {};
        text.resize(size);
        ifs.read(text.data(), static_cast<std::streamsize>(size));
        return { text };
    }

    std::vector<FileSystem::path> configHomes(string const& _programName)
    {
        std::vector<FileSystem::path> paths;

#if defined(CONTOUR_PROJECT_SOURCE_DIR) && !defined(NDEBUG)
        paths.emplace_back(FileSystem::path(CONTOUR_PROJECT_SOURCE_DIR) / "src" / "contour" / "display"
                           / "shaders");
#endif

        paths.emplace_back(configHome(_programName));

#if defined(__unix__) || defined(__APPLE__)
        paths.emplace_back(FileSystem::path("/etc") / _programName);
#endif

        return paths;
    }

    optional<terminal::Key> parseKey(string const& _name)
    {
        using terminal::Key;
        auto static constexpr mappings = array { pair { "F1"sv, Key::F1 },
                                                 pair { "F2"sv, Key::F2 },
                                                 pair { "F3"sv, Key::F3 },
                                                 pair { "F4"sv, Key::F4 },
                                                 pair { "F5"sv, Key::F5 },
                                                 pair { "F6"sv, Key::F6 },
                                                 pair { "F7"sv, Key::F7 },
                                                 pair { "F8"sv, Key::F8 },
                                                 pair { "F9"sv, Key::F9 },
                                                 pair { "F10"sv, Key::F10 },
                                                 pair { "F11"sv, Key::F11 },
                                                 pair { "F12"sv, Key::F12 },
                                                 pair { "DownArrow"sv, Key::DownArrow },
                                                 pair { "LeftArrow"sv, Key::LeftArrow },
                                                 pair { "RightArrow"sv, Key::RightArrow },
                                                 pair { "UpArrow"sv, Key::UpArrow },
                                                 pair { "Insert"sv, Key::Insert },
                                                 pair { "Delete"sv, Key::Delete },
                                                 pair { "Home"sv, Key::Home },
                                                 pair { "End"sv, Key::End },
                                                 pair { "PageUp"sv, Key::PageUp },
                                                 pair { "PageDown"sv, Key::PageDown },
                                                 pair { "Numpad_NumLock"sv, Key::Numpad_NumLock },
                                                 pair { "Numpad_Divide"sv, Key::Numpad_Divide },
                                                 pair { "Numpad_Multiply"sv, Key::Numpad_Multiply },
                                                 pair { "Numpad_Subtract"sv, Key::Numpad_Subtract },
                                                 pair { "Numpad_CapsLock"sv, Key::Numpad_CapsLock },
                                                 pair { "Numpad_Add"sv, Key::Numpad_Add },
                                                 pair { "Numpad_Decimal"sv, Key::Numpad_Decimal },
                                                 pair { "Numpad_Enter"sv, Key::Numpad_Enter },
                                                 pair { "Numpad_Equal"sv, Key::Numpad_Equal },
                                                 pair { "Numpad_0"sv, Key::Numpad_0 },
                                                 pair { "Numpad_1"sv, Key::Numpad_1 },
                                                 pair { "Numpad_2"sv, Key::Numpad_2 },
                                                 pair { "Numpad_3"sv, Key::Numpad_3 },
                                                 pair { "Numpad_4"sv, Key::Numpad_4 },
                                                 pair { "Numpad_5"sv, Key::Numpad_5 },
                                                 pair { "Numpad_6"sv, Key::Numpad_6 },
                                                 pair { "Numpad_7"sv, Key::Numpad_7 },
                                                 pair { "Numpad_8"sv, Key::Numpad_8 },
                                                 pair { "Numpad_9"sv, Key::Numpad_9 } };

        auto const name = toLower(_name);

        for (auto const& mapping: mappings)
            if (name == toLower(mapping.first))
                return mapping.second;

        return nullopt;
    }

    optional<variant<terminal::Key, char32_t>> parseKeyOrChar(string const& _name)
    {
        using namespace terminal::ControlCode;

        if (auto const key = parseKey(_name); key.has_value())
            return key.value();

        auto const text = QString::fromUtf8(_name.c_str()).toUcs4();
        if (text.size() == 1)
            return static_cast<char32_t>(toupper(static_cast<int>(text[0])));

        auto constexpr namedChars = array { pair { "ENTER"sv, (char) C0::CR },
                                            pair { "BACKSPACE"sv, (char) C0::BS },
                                            pair { "TAB"sv, (char) C0::HT },
                                            pair { "ESCAPE"sv, (char) C0::ESC },

                                            pair { "LESS"sv, '<' },
                                            pair { "GREATER"sv, '>' },
                                            pair { "PLUS"sv, '+' },

                                            pair { "APOSTROPHE"sv, '\'' },
                                            pair { "ADD"sv, '+' },
                                            pair { "BACKSLASH"sv, 'x' },
                                            pair { "COMMA"sv, ',' },
                                            pair { "DECIMAL"sv, '.' },
                                            pair { "DIVIDE"sv, '/' },
                                            pair { "EQUAL"sv, '=' },
                                            pair { "LEFT_BRACKET"sv, '[' },
                                            pair { "MINUS"sv, '-' },
                                            pair { "MULTIPLY"sv, '*' },
                                            pair { "PERIOD"sv, '.' },
                                            pair { "RIGHT_BRACKET"sv, ']' },
                                            pair { "SEMICOLON"sv, ';' },
                                            pair { "SLASH"sv, '/' },
                                            pair { "SUBTRACT"sv, '-' },
                                            pair { "SPACE"sv, ' ' } };

        auto const name = toUpper(_name);
        for (auto const& mapping: namedChars)
            if (name == mapping.first)
                return static_cast<char32_t>(mapping.second);

        return nullopt;
    }

    void parseCursorConfig(CursorConfig& cursorConfig,
                           YAML::Node const& rootNode,
                           UsedKeys& usedKeys,
                           std::string const& basePath)
    {
        if (!rootNode)
            return;

        std::string strValue;
        tryLoadChildRelative(usedKeys, rootNode, basePath, "shape", strValue, errorlog());
        if (!strValue.empty())
            cursorConfig.cursorShape = terminal::makeCursorShape(strValue);

        bool boolValue = cursorConfig.cursorDisplay == terminal::CursorDisplay::Blink;
        tryLoadChildRelative(usedKeys, rootNode, basePath, "blinking", boolValue, errorlog());
        cursorConfig.cursorDisplay =
            boolValue ? terminal::CursorDisplay::Blink : terminal::CursorDisplay::Steady;

        auto uintValue = cursorConfig.cursorBlinkInterval.count();
        tryLoadChildRelative(usedKeys, rootNode, basePath, "blinking_interval", uintValue, errorlog());
        cursorConfig.cursorBlinkInterval = chrono::milliseconds(uintValue);
    }

    optional<terminal::Modifier::Key> parseModifierKey(string const& _key)
    {
        using terminal::Modifier;
        auto const key = toUpper(_key);
        if (key == "ALT")
            return Modifier::Key::Alt;
        if (key == "CONTROL")
            return Modifier::Key::Control;
        if (key == "SHIFT")
            return Modifier::Key::Shift;
        if (key == "META")
            return Modifier::Key::Meta;
        return nullopt;
    }

    optional<terminal::MatchModes> parseMatchModes(UsedKeys& _usedKeys,
                                                   string const& _prefix,
                                                   YAML::Node const& _node)
    {
        using terminal::MatchModes;
        if (!_node)
            return terminal::MatchModes {};
        _usedKeys.emplace(_prefix);
        if (!_node.IsScalar())
            return nullopt;

        auto matchModes = MatchModes {};

        auto const modeStr = _node.as<string>();
        auto const args = crispy::split(modeStr, '|');
        for (string_view arg: args)
        {
            if (arg.empty())
                continue;
            bool negate = false;
            if (arg.front() == '~')
            {
                negate = true;
                arg.remove_prefix(1);
            }

            MatchModes::Flag flag = MatchModes::Flag::Default;
            string const upperArg = toUpper(arg);
            if (upperArg == "ALT"sv)
                flag = MatchModes::AlternateScreen;
            else if (upperArg == "APPCURSOR")
                flag = MatchModes::AppCursor;
            else if (upperArg == "APPKEYPAD")
                flag = MatchModes::AppKeypad;
            else if (upperArg == "INSERT")
                flag = MatchModes::Insert;
            else if (upperArg == "SELECT")
                flag = MatchModes::Select;
            else if (upperArg == "SEARCH")
                flag = MatchModes::Search;
            else if (upperArg == "TRACE")
                flag = MatchModes::Trace;
            else
            {
                errorlog()("Unknown input_mapping mode: {}", arg);
                continue;
            }

            if (negate)
                matchModes.disable(flag);
            else
                matchModes.enable(flag);
        }

        return matchModes;
    }

    optional<terminal::Modifier> parseModifier(UsedKeys& _usedKeys,
                                               string const& _prefix,
                                               YAML::Node const& _node)
    {
        using terminal::Modifier;
        if (!_node)
            return nullopt;
        _usedKeys.emplace(_prefix);
        if (_node.IsScalar())
            return parseModifierKey(_node.as<string>());
        if (!_node.IsSequence())
            return nullopt;

        terminal::Modifier mods;
        for (const auto& i: _node)
        {
            if (!i.IsScalar())
                return nullopt;

            auto const mod = parseModifierKey(i.as<string>());
            if (!mod)
                return nullopt;

            mods |= *mod;
        }
        return mods;
    }

    template <typename Input>
    void appendOrCreateBinding(vector<terminal::InputBinding<Input, ActionList>>& _bindings,
                               terminal::MatchModes _modes,
                               terminal::Modifier _modifier,
                               Input _input,
                               Action _action)
    {
        for (auto& binding: _bindings)
        {
            if (match(binding, _modes, _modifier, _input))
            {
                binding.binding.emplace_back(std::move(_action));
                return;
            }
        }

        _bindings.emplace_back(terminal::InputBinding<Input, ActionList> {
            _modes, _modifier, _input, ActionList { std::move(_action) } });
    }

    bool tryAddKey(InputMappings& _inputMappings,
                   terminal::MatchModes _modes,
                   terminal::Modifier _modifier,
                   YAML::Node const& _node,
                   Action _action)
    {
        if (!_node)
            return false;

        if (!_node.IsScalar())
            return false;

        auto const input = parseKeyOrChar(_node.as<string>());
        if (!input.has_value())
            return false;

        if (holds_alternative<terminal::Key>(*input))
        {
            appendOrCreateBinding(_inputMappings.keyMappings,
                                  _modes,
                                  _modifier,
                                  get<terminal::Key>(*input),
                                  std::move(_action));
        }
        else if (holds_alternative<char32_t>(*input))
        {
            appendOrCreateBinding(
                _inputMappings.charMappings, _modes, _modifier, get<char32_t>(*input), std::move(_action));
        }
        else
            assert(false && "The impossible happened.");

        return true;
    }

    optional<terminal::MouseButton> parseMouseButton(YAML::Node const& _node)
    {
        if (!_node)
            return nullopt;

        if (!_node.IsScalar())
            return nullopt;

        auto constexpr static mappings = array {
            pair { "WHEELUP"sv, terminal::MouseButton::WheelUp },
            pair { "WHEELDOWN"sv, terminal::MouseButton::WheelDown },
            pair { "LEFT"sv, terminal::MouseButton::Left },
            pair { "MIDDLE"sv, terminal::MouseButton::Middle },
            pair { "RIGHT"sv, terminal::MouseButton::Right },
        };
        auto const name = toUpper(_node.as<string>());
        for (auto const& mapping: mappings)
            if (name == mapping.first)
                return mapping.second;
        return nullopt;
    }

    bool tryAddMouse(vector<MouseInputMapping>& _bindings,
                     terminal::MatchModes _modes,
                     terminal::Modifier _modifier,
                     YAML::Node const& _node,
                     Action _action)
    {
        auto mouseButton = parseMouseButton(_node);
        if (!mouseButton)
            return false;

        appendOrCreateBinding(_bindings, _modes, _modifier, *mouseButton, std::move(_action));
        return true;
    }

    optional<Action> parseAction(UsedKeys& _usedKeys, string const& _prefix, YAML::Node const& _parent)
    {
        _usedKeys.emplace(_prefix + ".action");

        auto actionName = _parent["action"].as<string>();
        _usedKeys.emplace(_prefix + ".action." + actionName);
        auto actionOpt = actions::fromString(actionName);
        if (!actionOpt)
        {
            errorlog()("Unknown action '{}'.", _parent["action"].as<string>());
            return nullopt;
        }

        auto action = actionOpt.value();

        if (holds_alternative<actions::ChangeProfile>(action))
        {
            if (auto name = _parent["name"]; name.IsScalar())
            {
                _usedKeys.emplace(_prefix + ".name");
                return actions::ChangeProfile { name.as<string>() };
            }
            else
                return nullopt;
        }

        if (holds_alternative<actions::NewTerminal>(action))
        {
            if (auto profile = _parent["profile"]; profile && profile.IsScalar())
            {
                _usedKeys.emplace(_prefix + ".profile");
                return actions::NewTerminal { profile.as<string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::ReloadConfig>(action))
        {
            _usedKeys.emplace(_prefix + ".profile");
            if (auto profileName = _parent["profile"]; profileName.IsScalar())
            {
                _usedKeys.emplace(_prefix + ".profile");
                return actions::ReloadConfig { profileName.as<string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::SendChars>(action))
        {
            if (auto chars = _parent["chars"]; chars.IsScalar())
            {
                _usedKeys.emplace(_prefix + ".chars");
                return actions::SendChars { unescape(chars.as<string>()) };
            }
            else
                return nullopt;
        }

        if (holds_alternative<actions::CopySelection>(action))
        {
            if (auto node = _parent["format"]; node && node.IsScalar())
            {
                _usedKeys.emplace(_prefix + ".format");
                auto const formatString = toUpper(node.as<string>());
                static auto constexpr mappings =
                    std::array<std::pair<std::string_view, actions::CopyFormat>, 4> { {
                        { "TEXT", actions::CopyFormat::Text },
                        { "HTML", actions::CopyFormat::HTML },
                        { "PNG", actions::CopyFormat::PNG },
                        { "VT", actions::CopyFormat::VT },
                    } };
                if (auto p = std::find_if(mappings.begin(),
                                          mappings.end(),
                                          [&](auto const& t) { return t.first == formatString; });
                    p != mappings.end())
                {
                    return actions::CopySelection { p->second };
                }
                errorlog()("Invalid format '{}' in CopySelection action. Defaulting to 'text'.",
                           node.as<string>());
                return actions::CopySelection { actions::CopyFormat::Text };
            }
        }

        if (holds_alternative<actions::PasteClipboard>(action))
        {
            if (auto node = _parent["strip"]; node && node.IsScalar())
            {
                _usedKeys.emplace(_prefix + ".strip");
                return actions::PasteClipboard { node.as<bool>() };
            }
        }

        if (holds_alternative<actions::WriteScreen>(action))
        {
            if (auto chars = _parent["chars"]; chars.IsScalar())
            {
                _usedKeys.emplace(_prefix + ".chars");
                return actions::WriteScreen { unescape(chars.as<string>()) };
            }
            else
                return nullopt;
        }

        return action;
    }

    void parseInputMapping(UsedKeys& _usedKeys,
                           string const& _prefix,
                           Config& _config,
                           YAML::Node const& _mapping)
    {
        using namespace terminal;

        auto const action = parseAction(_usedKeys, _prefix, _mapping);
        auto const mods = parseModifier(_usedKeys, _prefix + ".mods", _mapping["mods"]);
        auto const mode = parseMatchModes(_usedKeys, _prefix + ".mode", _mapping["mode"]);
        if (action && mods && mode)
        {
            if (tryAddKey(_config.inputMappings, *mode, *mods, _mapping["key"], *action))
            {
                _usedKeys.emplace(_prefix + ".key");
            }
            else if (tryAddMouse(
                         _config.inputMappings.mouseMappings, *mode, *mods, _mapping["mouse"], *action))
            {
                _usedKeys.emplace(_prefix + ".mouse");
            }
            else
            {
                // TODO: log error: invalid key mapping at: _mapping.sourceLocation()
                ConfigLog()("Could not add some input mapping.");
            }
        }
    }

    void updateColorScheme(terminal::ColorPalette& colors,
                           UsedKeys& _usedKeys,
                           string const& _basePath,
                           YAML::Node const& _node)

    {
        if (!_node)
            return;
        ;

        _usedKeys.emplace(_basePath);
        using terminal::RGBColor;
        if (auto def = _node["default"]; def)
        {
            _usedKeys.emplace(_basePath + ".default");
            if (auto fg = def["foreground"]; fg)
            {
                _usedKeys.emplace(_basePath + ".default.foreground");
                colors.defaultForeground = fg.as<string>();
            }
            if (auto bg = def["background"]; bg)
            {
                _usedKeys.emplace(_basePath + ".default.background");
                colors.defaultBackground = bg.as<string>();
            }
        }

        if (auto p = parseCellRGBColorAndAlphaPair(_usedKeys, _basePath, _node, "search_highlight"))
            colors.searchHighlight = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(_usedKeys, _basePath, _node, "search_highlight_focused"))
            colors.searchHighlightFocused = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(_usedKeys, _basePath, _node, "word_highlight_current"))
            colors.wordHighlightCurrent = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(_usedKeys, _basePath, _node, "word_highlight_other"))
            colors.wordHighlight = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(_usedKeys, _basePath, _node, "selection"))
            colors.selection = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(_usedKeys, _basePath, _node, "vi_mode_highlight"))
            colors.yankHighlight = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(_usedKeys, _basePath, _node, "vi_mode_cursorline"))
            colors.normalModeCursorline = p.value();

        if (auto p = parseRGBColorPair(
                _usedKeys, _basePath, _node, "indicator_statusline", colors.indicatorStatusLine))
            colors.indicatorStatusLine = p.value();

        if (auto p = parseRGBColorPair(_usedKeys,
                                       _basePath,
                                       _node,
                                       "indicator_statusline_inactive",
                                       colors.indicatorStatusLineInactive))
            colors.indicatorStatusLineInactive = p.value();

        if (auto const p = parseRGBColorPair(
                _usedKeys, _basePath, _node, "input_method_editor", colors.inputMethodEditor))
            colors.inputMethodEditor = p.value();

        if (auto cursor = _node["cursor"]; cursor)
        {
            _usedKeys.emplace(_basePath + ".cursor");
            if (cursor.IsMap())
            {
                if (auto color = cursor["default"]; color.IsScalar())
                {
                    _usedKeys.emplace(_basePath + ".cursor.default");
                    colors.cursor.color = parseCellColor(color.as<string>());
                }
                if (auto color = cursor["text"]; color.IsScalar())
                {
                    _usedKeys.emplace(_basePath + ".cursor.text");
                    colors.cursor.textOverrideColor = parseCellColor(color.as<string>());
                }
            }
            else if (cursor.IsScalar())
            {
                errorlog()(
                    "Deprecated cursor config colorscheme entry. Please update your colorscheme entry for "
                    "cursor.");
                colors.cursor.color = RGBColor(cursor.as<string>());
            }
            else
                errorlog()("Invalid cursor config colorscheme entry.");
        }

        if (auto hyperlink = _node["hyperlink_decoration"]; hyperlink)
        {
            _usedKeys.emplace(_basePath + ".hyperlink_decoration");
            if (auto color = hyperlink["normal"]; color && color.IsScalar() && !color.as<string>().empty())
            {
                _usedKeys.emplace(_basePath + ".hyperlink_decoration.normal");
                colors.hyperlinkDecoration.normal = color.as<string>();
            }

            if (auto color = hyperlink["hover"]; color && color.IsScalar() && !color.as<string>().empty())
            {
                _usedKeys.emplace(_basePath + ".hyperlink_decoration.hover");
                colors.hyperlinkDecoration.hover = color.as<string>();
            }
        }

        auto const loadColorMap = [&](YAML::Node const& _parent, string const& _key, size_t _offset) -> bool {
            auto node = _parent[_key];
            if (!node)
                return false;

            auto const colorKeyPath = fmt::format("{}.{}", _basePath, _key);
            _usedKeys.emplace(colorKeyPath);
            if (node.IsMap())
            {
                auto const assignColor = [&](size_t _index, string const& _name) {
                    if (auto nodeValue = node[_name]; nodeValue)
                    {
                        _usedKeys.emplace(fmt::format("{}.{}", colorKeyPath, _name));
                        if (auto const value = nodeValue.as<string>(); !value.empty())
                        {
                            if (value[0] == '#')
                                colors.palette[_offset + _index] = value;
                            else if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
                                colors.palette[_offset + _index] = RGBColor { nodeValue.as<uint32_t>() };
                        }
                    }
                };
                assignColor(0, "black");
                assignColor(1, "red");
                assignColor(2, "green");
                assignColor(3, "yellow");
                assignColor(4, "blue");
                assignColor(5, "magenta");
                assignColor(6, "cyan");
                assignColor(7, "white");
                return true;
            }
            else if (node.IsSequence())
            {
                for (size_t i = 0; i < node.size() && i < 8; ++i)
                    if (node[i].IsScalar())
                        colors.palette[i] = RGBColor { node[i].as<uint32_t>() };
                    else
                        colors.palette[i] = RGBColor { node[i].as<string>() };
                return true;
            }
            return false;
        };

        loadColorMap(_node, "normal", 0);
        loadColorMap(_node, "bright", 8);
        if (!loadColorMap(_node, "dim", 256))
        {
            // calculate dim colors based on normal colors
            for (unsigned i = 0; i < 8; ++i)
                colors.palette[256 + i] = colors.palette[i] * 0.5f;
        }

        // TODO: color palette from 16..255

        float opacityValue = 1.0;
        tryLoadChildRelative(
            _usedKeys, _node, _basePath, "background_image.opacity", opacityValue, errorlog());

        bool imageBlur = false;
        tryLoadChildRelative(_usedKeys, _node, _basePath, "background_image.blur", imageBlur, errorlog());

        string fileName;
        if (tryLoadChildRelative(_usedKeys, _node, _basePath, "background_image.path", fileName, errorlog()))
            colors.backgroundImage = loadImage(fileName, opacityValue, imageBlur);
    }

    terminal::ColorPalette loadColorScheme(UsedKeys& _usedKeys,
                                           string const& _basePath,
                                           YAML::Node const& _node)
    {

        terminal::ColorPalette colors;
        updateColorScheme(colors, _usedKeys, _basePath, _node);
        return colors;
    }

    void softLoadFont(UsedKeys& _usedKeys,
                      string_view _basePath,
                      YAML::Node const& _node,
                      text::font_description& _store)
    {
        if (_node.IsScalar())
        {
            _store.familyName = _node.as<string>();
            _usedKeys.emplace(_basePath);
        }
        else if (_node.IsMap())
        {
            _usedKeys.emplace(_basePath);

            if (_node["family"].IsScalar())
            {
                _usedKeys.emplace(fmt::format("{}.{}", _basePath, "family"));
                _store.familyName = _node["family"].as<string>();
            }

            if (_node["slant"] && _node["slant"].IsScalar())
            {
                _usedKeys.emplace(fmt::format("{}.{}", _basePath, "slant"));
                if (auto const p = text::make_font_slant(_node["slant"].as<string>()))
                    _store.slant = p.value();
            }

            if (_node["weight"] && _node["weight"].IsScalar())
            {
                _usedKeys.emplace(fmt::format("{}.{}", _basePath, "weight"));
                if (auto const p = text::make_font_weight(_node["weight"].as<string>()))
                    _store.weight = p.value();
            }

            if (_node["features"] && _node["features"] && _node["features"].IsSequence())
            {
                _usedKeys.emplace(fmt::format("{}.{}", _basePath, "features"));
                YAML::Node featuresNode = _node["features"];
                for (auto&& i: featuresNode)
                {
                    auto const featureNode = i;
                    if (!featureNode.IsScalar())
                    {
                        errorlog()("Invalid font feature \"{}\".", featureNode.as<string>());
                        continue;
                    }

                    // Feature can be either 4 letter code or optionally ending with - to denote disabling it.
                    auto const [tag, enabled] = [&]() -> tuple<string, bool> {
                        auto value = featureNode.as<string>();
                        if (!value.empty())
                        {
                            if (value[0] == '+')
                                return { value.substr(1), true };
                            if (value[0] == '-')
                                return { value.substr(1), false };
                        }
                        return { std::move(value), true };
                    }();

                    if (tag.size() != 4)
                    {
                        errorlog()(
                            "Invalid font feature \"{}\". Font features are denoted as 4-letter codes.",
                            featureNode.as<string>());
                        continue;
                    }
                    _store.features.emplace_back(tag[0], tag[1], tag[2], tag[3], enabled);
                }
            }
        }
    }

    void softLoadFont(terminal::rasterizer::TextShapingEngine _textShapingEngine,
                      UsedKeys& _usedKeys,
                      string_view _basePath,
                      YAML::Node const& _node,
                      string const& _key,
                      text::font_description& _store)
    {
        if (!_node)
            return;
        auto node = _node[_key];
        if (!node)
            return;

        softLoadFont(_usedKeys, fmt::format("{}.{}", _basePath, _key), node, _store);

        if (node.IsMap())
        {
            _usedKeys.emplace(fmt::format("{}.{}", _basePath, _key));
            if (node["features"].IsSequence())
            {
                using terminal::rasterizer::TextShapingEngine;
                switch (_textShapingEngine)
                {
                    case TextShapingEngine::OpenShaper: break;
                    case TextShapingEngine::CoreText:
                    case TextShapingEngine::DWrite:
                        // TODO: Implement font feature settings handling for these engines.
                        errorlog()("The configured text shaping engine {} does not yet support font feature "
                                   "settings. Ignoring.",
                                   _textShapingEngine);
                }
            }
        }
    }

    template <typename T>
    bool sanitizeRange(std::reference_wrapper<T> _value, T _min, T _max)
    {
        if (_min <= _value.get() && _value.get() <= _max)
            return true;

        _value.get() = std::clamp(_value.get(), _min, _max);
        return false;
    }

    optional<terminal::VTType> stringToVTType(std::string const& _value)
    {
        using Type = terminal::VTType;
        auto constexpr static mappings = array<tuple<string_view, terminal::VTType>, 10> {
            tuple { "VT100"sv, Type::VT100 }, tuple { "VT220"sv, Type::VT220 },
            tuple { "VT240"sv, Type::VT240 }, tuple { "VT330"sv, Type::VT330 },
            tuple { "VT340"sv, Type::VT340 }, tuple { "VT320"sv, Type::VT320 },
            tuple { "VT420"sv, Type::VT420 }, tuple { "VT510"sv, Type::VT510 },
            tuple { "VT520"sv, Type::VT520 }, tuple { "VT525"sv, Type::VT525 }
        };
        for (auto const& mapping: mappings)
            if (get<0>(mapping) == _value)
                return get<1>(mapping);
        return nullopt;
    }

    void updateTerminalProfile(TerminalProfile& profile,
                               UsedKeys& _usedKeys,
                               YAML::Node const& _profile,
                               std::string const& _parentPath,
                               std::string const& _profileName,
                               unordered_map<string, terminal::ColorPalette> const& _colorschemes,
                               logstore::MessageBuilder _logger)
    {

        if (auto colors = _profile["colors"]; colors) // {{{
        {
            _usedKeys.emplace(fmt::format("{}.{}.colors", _parentPath, _profileName));
            auto const path = fmt::format("{}.{}.{}", _parentPath, _profileName, "colors");
            if (colors.IsMap())
                profile.colors = loadColorScheme(_usedKeys, path, colors);
            else if (auto i = _colorschemes.find(colors.as<string>()); i != _colorschemes.end())
            {
                _usedKeys.emplace(path);
                profile.colors = i->second;
            }
            else if (colors.IsScalar())
            {
                bool found = false;
                for (FileSystem::path const& prefix: configHomes("contour"))
                {
                    auto const filePath = prefix / "colorschemes" / (colors.as<string>() + ".yml");
                    auto fileContents = readFile(filePath);
                    if (!fileContents)
                        continue;
                    YAML::Node subDocument = YAML::Load(fileContents.value());
                    UsedKeys usedColorKeys;
                    profile.colors = loadColorScheme(usedColorKeys, "", subDocument);
                    // TODO: Check usedColorKeys for validity.
                    ConfigLog()("Loaded colors from {}.", filePath.string());
                    found = true;
                    break;
                }
                if (!found)
                    _logger("Could not open colorscheme file for \"{}\".", colors.as<string>());
            }
            else
                _logger("scheme '{}' not found.", colors.as<string>());
        }
        else
            _logger("No colors section in profile {} found.", _profileName);
        // }}}

        string const basePath = fmt::format("{}.{}", _parentPath, _profileName);
        tryLoadChildRelative(
            _usedKeys, _profile, basePath, "escape_sandbox", profile.shell.escapeSandbox, _logger);
        tryLoadChildRelative(_usedKeys, _profile, basePath, "shell", profile.shell.program, _logger);
        if (profile.shell.program.empty())
        {
            if (!profile.shell.arguments.empty())
                _logger("No shell defined but arguments. Ignoring arguments.");

            auto loginShell = Process::loginShell(profile.shell.escapeSandbox);
            profile.shell.program = loginShell.front();
            loginShell.erase(loginShell.begin());
            profile.shell.arguments = loginShell;
        }
        tryLoadChildRelative(_usedKeys, _profile, basePath, "maximized", profile.maximized, _logger);
        tryLoadChildRelative(_usedKeys, _profile, basePath, "fullscreen", profile.fullscreen, _logger);
        tryLoadChildRelative(
            _usedKeys, _profile, basePath, "refresh_rate", profile.refreshRate.value, _logger);
        tryLoadChildRelative(_usedKeys,
                             _profile,
                             basePath,
                             "copy_last_mark_range_offset",
                             profile.copyLastMarkRangeOffset,
                             _logger);
        tryLoadChildRelative(
            _usedKeys, _profile, basePath, "show_title_bar", profile.show_title_bar, _logger);
        tryLoadChildRelative(_usedKeys,
                             _profile,
                             basePath,
                             "draw_bold_text_with_bright_colors",
                             profile.colors.useBrightColors,
                             _logger);
        tryLoadChildRelative(_usedKeys, _profile, basePath, "wm_class", profile.wmClass, _logger);

        if (auto args = _profile["arguments"]; args && args.IsSequence())
        {
            _usedKeys.emplace(fmt::format("{}.arguments", basePath));
            for (auto const& argNode: args)
                profile.shell.arguments.emplace_back(argNode.as<string>());
        }

        std::string strValue;
        tryLoadChildRelative(_usedKeys, _profile, basePath, "initial_working_directory", strValue, _logger);
        if (!strValue.empty())
            profile.shell.workingDirectory = FileSystem::path(strValue);

        profile.shell.workingDirectory =
            homeResolvedPath(profile.shell.workingDirectory.generic_string(), Process::homeDirectory());

        profile.shell.env["TERMINAL_NAME"] = "contour";
        profile.shell.env["TERMINAL_VERSION_TRIPLE"] =
            fmt::format("{}.{}.{}", CONTOUR_VERSION_MAJOR, CONTOUR_VERSION_MINOR, CONTOUR_VERSION_PATCH);
        profile.shell.env["TERMINAL_VERSION_STRING"] = CONTOUR_VERSION_STRING;

        std::optional<FileSystem::path> appTerminfoDir;
#if defined(__APPLE__)
        {
            char buf[1024];
            uint32_t len = sizeof(buf);
            if (_NSGetExecutablePath(buf, &len) == 0)
            {
                auto p = FileSystem::path(buf).parent_path().parent_path() / "Resources" / "terminfo";
                if (FileSystem::is_directory(p))
                {
                    appTerminfoDir = p;
                    profile.shell.env["TERMINFO_DIRS"] = p.string();
                }
            }
        }
#endif

        if (auto env = _profile["environment"]; env)
        {
            auto const envpath = basePath + ".environment";
            _usedKeys.emplace(envpath);
            for (auto i = env.begin(); i != env.end(); ++i)
            {
                auto const name = i->first.as<string>();
                auto const value = i->second.as<string>();
                _usedKeys.emplace(fmt::format("{}.{}", envpath, name));
                profile.shell.env[name] = value;
            }
        }

        // force some default env
        if (profile.shell.env.find("TERM") == profile.shell.env.end())
        {
            profile.shell.env["TERM"] = getDefaultTERM(appTerminfoDir);
            ConfigLog()("Defaulting TERM to {}.", profile.shell.env["TERM"]);
        }

        if (profile.shell.env.find("COLORTERM") == profile.shell.env.end())
            profile.shell.env["COLORTERM"] = "truecolor";

        strValue = fmt::format("{}", profile.terminalId);
        tryLoadChildRelative(_usedKeys, _profile, basePath, "terminal_id", strValue, _logger);
        if (auto const idOpt = stringToVTType(strValue))
            profile.terminalId = idOpt.value();
        else
            _logger("Invalid Terminal ID \"{}\", specified", strValue);

        tryLoadChildRelative(
            _usedKeys, _profile, basePath, "terminal_size.columns", profile.terminalSize.columns, _logger);
        tryLoadChildRelative(
            _usedKeys, _profile, basePath, "terminal_size.lines", profile.terminalSize.lines, _logger);
        {
            auto constexpr MinimalTerminalSize = PageSize { LineCount(3), ColumnCount(3) };
            auto constexpr MaximumTerminalSize = PageSize { LineCount(200), ColumnCount(300) };

            if (!sanitizeRange(ref(profile.terminalSize.columns.value),
                               *MinimalTerminalSize.columns,
                               *MaximumTerminalSize.columns))
                _logger("Terminal width {} out of bounds. Should be between {} and {}.",
                        profile.terminalSize.columns,
                        MinimalTerminalSize.columns,
                        MaximumTerminalSize.columns);

            if (!sanitizeRange(
                    ref(profile.terminalSize.lines), MinimalTerminalSize.lines, MaximumTerminalSize.lines))
                _logger("Terminal height {} out of bounds. Should be between {} and {}.",
                        profile.terminalSize.lines,
                        MinimalTerminalSize.lines,
                        MaximumTerminalSize.lines);
        }

        strValue = "ask";
        if (tryLoadChildRelative(
                _usedKeys, _profile, basePath, "permissions.capture_buffer", strValue, _logger))
        {
            if (auto x = toPermission(strValue))
                profile.permissions.captureBuffer = x.value();
        }

        strValue = "ask";
        if (tryLoadChildRelative(_usedKeys, _profile, basePath, "permissions.change_font", strValue, _logger))
        {
            if (auto x = toPermission(strValue))
                profile.permissions.changeFont = x.value();
        }

        strValue = "ask";
        if (tryLoadChildRelative(_usedKeys,
                                 _profile,
                                 basePath,
                                 "permissions.display_host_writable_statusline",
                                 strValue,
                                 _logger))
        {
            if (auto x = toPermission(strValue))
                profile.permissions.displayHostWritableStatusLine = x.value();
        }

        if (tryLoadChildRelative(_usedKeys, _profile, basePath, "font.size", profile.fonts.size.pt, _logger))
        {
            if (profile.fonts.size < MinimumFontSize)
            {
                _logger("Invalid font size {} set in config file. Minimum value is {}.",
                        profile.fonts.size,
                        MinimumFontSize);
                profile.fonts.size = MinimumFontSize;
            }
        }

        tryLoadChildRelative(_usedKeys,
                             _profile,
                             basePath,
                             "font.builtin_box_drawing",
                             profile.fonts.builtinBoxDrawing,
                             _logger);

        auto constexpr NativeTextShapingEngine =
#if defined(_WIN32)
            terminal::rasterizer::TextShapingEngine::DWrite;
#elif defined(__APPLE__)
            terminal::rasterizer::TextShapingEngine::CoreText;
#else
            terminal::rasterizer::TextShapingEngine::OpenShaper;
#endif

        auto constexpr NativeFontLocator =
#if defined(_WIN32)
            terminal::rasterizer::FontLocatorEngine::DWrite;
#elif defined(__APPLE__)
            terminal::rasterizer::FontLocatorEngine::CoreText;
#else
            terminal::rasterizer::FontLocatorEngine::FontConfig;
#endif

        strValue = fmt::format("{}", profile.fonts.textShapingEngine);
        if (tryLoadChildRelative(
                _usedKeys, _profile, basePath, "font.text_shaping.engine", strValue, _logger))
        {
            auto const lwrValue = toLower(strValue);
            if (lwrValue == "dwrite" || lwrValue == "directwrite")
                profile.fonts.textShapingEngine = terminal::rasterizer::TextShapingEngine::DWrite;
            else if (lwrValue == "core" || lwrValue == "coretext")
                profile.fonts.textShapingEngine = terminal::rasterizer::TextShapingEngine::CoreText;
            else if (lwrValue == "open" || lwrValue == "openshaper")
                profile.fonts.textShapingEngine = terminal::rasterizer::TextShapingEngine::OpenShaper;
            else if (lwrValue == "native")
                profile.fonts.textShapingEngine = NativeTextShapingEngine;
            else
                ConfigLog()("Invalid value for configuration key {}.font.text_shaping.engine: {}",
                            basePath,
                            strValue);
        }

        profile.fonts.fontLocator = NativeFontLocator;
        strValue = fmt::format("{}", profile.fonts.fontLocator);
        if (tryLoadChildRelative(_usedKeys, _profile, basePath, "font.locator", strValue, _logger))
        {
            auto const lwrValue = toLower(strValue);
            if (lwrValue == "fontconfig")
                profile.fonts.fontLocator = terminal::rasterizer::FontLocatorEngine::FontConfig;
            else if (lwrValue == "coretext")
                profile.fonts.fontLocator = terminal::rasterizer::FontLocatorEngine::CoreText;
            else if (lwrValue == "dwrite" || lwrValue == "directwrite")
                profile.fonts.fontLocator = terminal::rasterizer::FontLocatorEngine::DWrite;
            else if (lwrValue == "native")
                profile.fonts.fontLocator = NativeFontLocator;
            else if (lwrValue == "mock")
                profile.fonts.fontLocator = terminal::rasterizer::FontLocatorEngine::Mock;
            else
                ConfigLog()("Invalid value for configuration key {}.font.locator: {}", basePath, strValue);
        }

        bool strictSpacing = false;
        tryLoadChildRelative(_usedKeys, _profile, basePath, "font.strict_spacing", strictSpacing, _logger);

        auto const fontBasePath = fmt::format("{}.{}.font", _parentPath, _profileName);

        softLoadFont(profile.fonts.textShapingEngine,
                     _usedKeys,
                     fontBasePath,
                     _profile["font"],
                     "regular",
                     profile.fonts.regular);

        profile.fonts.bold = profile.fonts.regular;
        profile.fonts.bold.weight = text::font_weight::bold;
        softLoadFont(profile.fonts.textShapingEngine,
                     _usedKeys,
                     fontBasePath,
                     _profile["font"],
                     "bold",
                     profile.fonts.bold);

        profile.fonts.italic = profile.fonts.regular;
        profile.fonts.italic.slant = text::font_slant::italic;
        softLoadFont(profile.fonts.textShapingEngine,
                     _usedKeys,
                     fontBasePath,
                     _profile["font"],
                     "italic",
                     profile.fonts.italic);

        profile.fonts.boldItalic.weight = text::font_weight::bold;
        profile.fonts.boldItalic.slant = text::font_slant::italic;
        softLoadFont(profile.fonts.textShapingEngine,
                     _usedKeys,
                     fontBasePath,
                     _profile["font"],
                     "bold_italic",
                     profile.fonts.boldItalic);

        profile.fonts.emoji.familyName = "emoji";
        profile.fonts.emoji.spacing = text::font_spacing::mono;
        softLoadFont(profile.fonts.textShapingEngine,
                     _usedKeys,
                     fontBasePath,
                     _profile["font"],
                     "emoji",
                     profile.fonts.emoji);

#if defined(_WIN32)
        // Windows does not understand font family "emoji", but fontconfig does. Rewrite user-input here.
        if (profile.fonts.emoji.familyName == "emoji")
            profile.fonts.emoji.familyName = "Segoe UI Emoji";
#endif

        strValue = "gray";
        tryLoadChildRelative(_usedKeys, _profile, basePath, "font.render_mode", strValue, _logger);
        auto const static renderModeMap = array {
            pair { "lcd"sv, text::render_mode::lcd },           pair { "light"sv, text::render_mode::light },
            pair { "gray"sv, text::render_mode::gray },         pair { ""sv, text::render_mode::gray },
            pair { "monochrome"sv, text::render_mode::bitmap },
        };

        if (auto i = crispy::find_if(renderModeMap, [&](auto m) { return m.first == strValue; });
            i != renderModeMap.end())
            profile.fonts.renderMode = i->second;
        else
            _logger("Invalid render_mode \"{}\" in configuration.", strValue);

        auto intValue = LineCount();
        tryLoadChildRelative(_usedKeys, _profile, basePath, "history.limit", intValue, _logger);
        // value -1 is used for infinite grid
        if (unbox<int>(intValue) == -1)
            profile.maxHistoryLineCount = Infinite();
        else if (unbox<int>(intValue) > -1)
            profile.maxHistoryLineCount = LineCount(intValue);
        else
            profile.maxHistoryLineCount = LineCount(0);

        strValue = fmt::format("{}", ScrollBarPosition::Right);
        if (tryLoadChildRelative(_usedKeys, _profile, basePath, "scrollbar.position", strValue, _logger))
        {
            auto const literal = toLower(strValue);
            if (literal == "left")
                profile.scrollbarPosition = ScrollBarPosition::Left;
            else if (literal == "right")
                profile.scrollbarPosition = ScrollBarPosition::Right;
            else if (literal == "hidden")
                profile.scrollbarPosition = ScrollBarPosition::Hidden;
            else
                _logger("Invalid value for config entry {}: {}", "scrollbar.position", strValue);
        }
        tryLoadChildRelative(_usedKeys,
                             _profile,
                             basePath,
                             "scrollbar.hide_in_alt_screen",
                             profile.hideScrollbarInAltScreen,
                             _logger);

        tryLoadChildRelative(_usedKeys,
                             _profile,
                             basePath,
                             "mouse.hide_while_typing",
                             profile.mouse_hide_while_typing,
                             _logger);

        tryLoadChildRelative(_usedKeys,
                             _profile,
                             basePath,
                             "history.auto_scroll_on_update",
                             profile.autoScrollOnUpdate,
                             _logger);
        tryLoadChildRelative(_usedKeys,
                             _profile,
                             basePath,
                             "history.scroll_multiplier",
                             profile.historyScrollMultiplier,
                             _logger);

        float floatValue = 1.0;
        tryLoadChildRelative(_usedKeys, _profile, basePath, "background.opacity", floatValue, _logger);
        profile.backgroundOpacity =
            (terminal::Opacity)(static_cast<unsigned>(255 * clamp(floatValue, 0.0f, 1.0f)));
        tryLoadChildRelative(
            _usedKeys, _profile, basePath, "background.blur", profile.backgroundBlur, _logger);

        strValue = "dotted-underline"; // TODO: fmt::format("{}", profile.hyperlinkDecoration.normal);
        tryLoadChildRelative(_usedKeys, _profile, basePath, "hyperlink_decoration.normal", strValue, _logger);
        if (auto const pdeco = terminal::rasterizer::to_decorator(strValue); pdeco.has_value())
            profile.hyperlinkDecoration.normal = *pdeco;

        strValue = "underline"; // TODO: fmt::format("{}", profile.hyperlinkDecoration.hover);
        tryLoadChildRelative(_usedKeys, _profile, basePath, "hyperlink_decoration.hover", strValue, _logger);

        tryLoadChildRelative(
            _usedKeys, _profile, basePath, "vi_mode_scrolloff", profile.modalCursorScrollOff, _logger);

        auto uintValue = profile.highlightTimeout.count();
        tryLoadChildRelative(_usedKeys, _profile, basePath, "vi_mode_highlight_timeout", uintValue, _logger);
        profile.highlightTimeout = chrono::milliseconds(uintValue);
        if (auto const pdeco = terminal::rasterizer::to_decorator(strValue); pdeco.has_value())
            profile.hyperlinkDecoration.hover = *pdeco;

        tryLoadChildRelative(_usedKeys,
                             _profile,
                             basePath,
                             "highlight_word_and_matches_on_double_click",
                             profile.highlightDoubleClickedWord,
                             _logger);

        parseCursorConfig(
            profile.inputModes.insert.cursor, _profile["cursor"], _usedKeys, basePath + ".cursor");
        _usedKeys.emplace(basePath + ".cursor");

        if (auto normalModeNode = _profile["normal_mode"])
        {
            _usedKeys.emplace(basePath + ".normal_mode");
            parseCursorConfig(profile.inputModes.normal.cursor,
                              normalModeNode["cursor"],
                              _usedKeys,
                              basePath + ".normal_mode.cursor");
            _usedKeys.emplace(basePath + ".normal_mode.cursor");
        }

        if (auto visualModeNode = _profile["visual_mode"])
        {
            _usedKeys.emplace(basePath + ".visual_mode");
            parseCursorConfig(profile.inputModes.visual.cursor,
                              visualModeNode["cursor"],
                              _usedKeys,
                              basePath + ".visual_mode.cursor");
            _usedKeys.emplace(basePath + ".visual_mode.cursor");
        }

        strValue = "none";
        tryLoadChildRelative(_usedKeys, _profile, basePath, "status_line.display", strValue, _logger);
        if (strValue == "indicator")
            profile.initialStatusDisplayType = terminal::StatusDisplayType::Indicator;
        else if (strValue == "none")
            profile.initialStatusDisplayType = terminal::StatusDisplayType::None;
        else
            _logger("Invalid value for config entry {}: {}", "status_line.display", strValue);

        if (tryLoadChildRelative(_usedKeys, _profile, basePath, "status_line.position", strValue, _logger))
        {
            auto const literal = toLower(strValue);
            if (literal == "bottom")
                profile.statusDisplayPosition = terminal::StatusDisplayPosition::Bottom;
            else if (literal == "top")
                profile.statusDisplayPosition = terminal::StatusDisplayPosition::Top;
            else
                _logger("Invalid value for config entry {}: {}", "status_line.position", strValue);
        }

        bool boolValue = false;
        if (tryLoadChildRelative(
                _usedKeys, _profile, basePath, "status_line.sync_to_window_title", boolValue, _logger))
            profile.syncWindowTitleWithHostWritableStatusDisplay = boolValue;
    }

    TerminalProfile loadTerminalProfile(UsedKeys& _usedKeys,
                                        YAML::Node const& _profile,
                                        std::string const& _parentPath,
                                        std::string const& _profileName,
                                        unordered_map<string, terminal::ColorPalette> const& _colorschemes)
    {
        auto profile = TerminalProfile {}; // default profile
        updateTerminalProfile(
            profile, _usedKeys, _profile, _parentPath, _profileName, _colorschemes, errorlog());
        return profile;
    }

} // namespace
// }}}

FileSystem::path configHome(string const& _programName)
{
#if defined(__unix__) || defined(__APPLE__)
    if (auto const* value = getenv("XDG_CONFIG_HOME"); value && *value)
        return FileSystem::path { value } / _programName;
    else if (auto const* value = getenv("HOME"); value && *value)
        return FileSystem::path { value } / ".config" / _programName;
#endif

#if defined(_WIN32)
    DWORD size = GetEnvironmentVariableA("LOCALAPPDATA", nullptr, 0);
    if (size)
    {
        std::vector<char> buf;
        buf.resize(size);
        GetEnvironmentVariableA("LOCALAPPDATA", &buf[0], size);
        return FileSystem::path { &buf[0] } / _programName;
    }
#endif

    throw runtime_error { "Could not find config home folder." };
}

FileSystem::path configHome()
{
    return configHome("contour");
}

std::string defaultConfigString()
{
    QFile file(":/contour/contour.yml");
    file.open(QFile::ReadOnly);
    return file.readAll().toStdString();
}

error_code createDefaultConfig(FileSystem::path const& _path)
{
    FileSystemError ec;
    if (!_path.parent_path().empty())
    {
        FileSystem::create_directories(_path.parent_path(), ec);
        if (ec)
            return ec;
    }

    ofstream { _path.string(), ios::binary | ios::trunc } << defaultConfigString();

    return error_code {};
}

std::string defaultConfigFilePath()
{
    return (configHome() / "contour.yml").string();
}

Config loadConfig()
{
    return loadConfigFromFile(defaultConfigFilePath());
}

Config loadConfigFromFile(FileSystem::path const& _fileName)
{
    Config config {};
    loadConfigFromFile(config, _fileName);
    return config;
}

/**
 * @return success or failure of loading the config file.
 */
void loadConfigFromFile(Config& _config, FileSystem::path const& _fileName)
{
    auto logger = errorlog();
    ConfigLog()("Loading configuration from file: {}", _fileName.string());
    _config.backingFilePath = _fileName;
    createFileIfNotExists(_config.backingFilePath);
    auto usedKeys = UsedKeys {};
    YAML::Node doc;
    try
    {
        doc = YAML::LoadFile(_fileName.string());
    }
    catch (exception const& e)
    {
        errorlog()("Configuration file is corrupted. {}", e.what());
        auto newfileName = _fileName;
        newfileName.replace_filename("default_contour.yml");
        createDefaultConfig(newfileName);
        return loadConfigFromFile(_config, newfileName);
    }
    tryLoadValue(usedKeys, doc, "word_delimiters", _config.wordDelimiters, logger);

    if (auto opt =
            parseModifier(usedKeys, "bypass_mouse_protocol_modifier", doc["bypass_mouse_protocol_modifier"]);
        opt.has_value())
        _config.bypassMouseProtocolModifier = opt.value();

    if (auto opt =
            parseModifier(usedKeys, "mouse_block_selection_modifier", doc["mouse_block_selection_modifier"]);
        opt.has_value())
        _config.mouseBlockSelectionModifier = opt.value();

    if (doc["on_mouse_select"].IsDefined())
    {
        usedKeys.emplace("on_mouse_select");
        auto const value = toUpper(doc["on_mouse_select"].as<string>());
        auto constexpr mappings = array {
            pair { "COPYTOCLIPBOARD", SelectionAction::CopyToClipboard },
            pair { "COPYTOSELECTIONCLIPBOARD", SelectionAction::CopyToSelectionClipboard },
            pair { "NOTHING", SelectionAction::Nothing },
        };
        bool found = false;
        for (auto const& mapping: mappings)
            if (mapping.first == value)
            {
                _config.onMouseSelection = mapping.second;
                usedKeys.emplace("on_mouse_select");
                found = true;
                break;
            }
        if (!found)
            errorlog()("Invalid action specified for on_mouse_select: {}.", value);
    }

    auto constexpr KnownExperimentalFeatures = array<string_view, 0> {
        // "tcap"sv
    };

    if (auto experimental = doc["experimental"]; experimental.IsMap())
    {
        usedKeys.emplace("experimental");
        for (auto const& x: experimental)
        {
            auto const key = x.first.as<string>();
            if (crispy::count(KnownExperimentalFeatures, key) == 0)
            {
                errorlog()("Unknown experimental feature tag: {}.", key);
                continue;
            }

            usedKeys.emplace("experimental." + x.first.as<string>());
            if (!x.second.as<bool>())
                continue;

            errorlog()("Enabling experimental feature {}.", key);
            _config.experimentalFeatures.insert(key);
        }
    }

    tryLoadValue(usedKeys, doc, "spawn_new_process", _config.spawnNewProcess, logger);

    tryLoadValue(usedKeys, doc, "live_config", _config.live, logger);

    auto logEnabled = false;
    tryLoadValue(usedKeys, doc, "logging.enabled", logEnabled, logger);

    auto logFilePath = ""s;
    tryLoadValue(usedKeys, doc, "logging.file", logFilePath, logger);

    if (logEnabled)
    {
        logFilePath =
            homeResolvedPath(replaceVariables(logFilePath, VariableReplacer()), Process::homeDirectory())
                .generic_string();

        if (!logFilePath.empty())
        {
            _config.loggingSink = make_shared<logstore::Sink>(logEnabled, make_shared<ofstream>(logFilePath));
            logstore::set_sink(*_config.loggingSink);
        }
    }

    tryLoadValue(usedKeys, doc, "images.sixel_scrolling", _config.sixelScrolling, logger);
    tryLoadValue(usedKeys, doc, "images.sixel_register_count", _config.maxImageColorRegisters, logger);
    tryLoadValue(usedKeys, doc, "images.max_width", _config.maxImageSize.width, logger);
    tryLoadValue(usedKeys, doc, "images.max_height", _config.maxImageSize.height, logger);

    if (auto colorschemes = doc["color_schemes"]; colorschemes)
    {
        usedKeys.emplace("color_schemes");
        // load default colorschemes
        const std::string name_default = "default";
        auto const path_default = "color_schemes." + name_default;
        _config.colorschemes[name_default] =
            loadColorScheme(usedKeys, path_default, colorschemes.begin()->second);

        for (auto i = colorschemes.begin(); i != colorschemes.end(); ++i)
        {
            auto const name = i->first.as<string>();
            if (name == name_default)
                continue;
            auto const path = "color_schemes." + name;
            _config.colorschemes[name] = _config.colorschemes[name_default];
            updateColorScheme(_config.colorschemes[name], usedKeys, path, i->second);
        }
    }

    tryLoadValue(usedKeys, doc, "platform_plugin", _config.platformPlugin, logger);
    if (_config.platformPlugin == "auto")
        _config.platformPlugin = ""; // Mapping "auto" to its internally equivalent "".

    string renderingBackendStr;
    if (tryLoadValue(usedKeys, doc, "renderer.backend", renderingBackendStr, logger))
    {
        renderingBackendStr = toUpper(renderingBackendStr);
        if (renderingBackendStr == "OPENGL"sv)
            _config.renderingBackend = RenderingBackend::OpenGL;
        else if (renderingBackendStr == "SOFTWARE"sv)
            _config.renderingBackend = RenderingBackend::Software;
        else if (renderingBackendStr != ""sv && renderingBackendStr != "DEFAULT"sv)
            errorlog()("Unknown renderer: {}.", renderingBackendStr);
    }

    tryLoadValue(
        usedKeys, doc, "renderer.tile_hashtable_slots", _config.textureAtlasHashtableSlots.value, logger);
    tryLoadValue(usedKeys, doc, "renderer.tile_cache_count", _config.textureAtlasTileCount.value, logger);
    tryLoadValue(usedKeys, doc, "renderer.tile_direct_mapping", _config.textureAtlasDirectMapping, logger);

    if (doc["mock_font_locator"].IsSequence())
    {
        vector<text::font_description_and_source> registry;
        usedKeys.emplace("mock_font_locator");
        for (size_t i = 0; i < doc["mock_font_locator"].size(); ++i)
        {
            auto const node = doc["mock_font_locator"][i];
            auto const fontBasePath = fmt::format("mock_font_locator.{}", i);
            text::font_description_and_source fds;
            softLoadFont(usedKeys, fontBasePath, node, fds.description);
            fds.source = text::font_path { node["path"].as<string>() };
            usedKeys.emplace(fmt::format("{}.path", fontBasePath));
            registry.emplace_back(std::move(fds));
        }
        text::mock_font_locator::configure(std::move(registry));
    }

    tryLoadValue(usedKeys, doc, "read_buffer_size", _config.ptyReadBufferSize, logger);
    if ((_config.ptyReadBufferSize % 16) != 0)
    {
        // For improved performance ...
        ConfigLog()("read_buffer_size must be a multiple of 16.");
    }

    tryLoadValue(usedKeys, doc, "pty_buffer_size", _config.ptyBufferObjectSize, logger);
    if (_config.ptyBufferObjectSize < 1024 * 256)
    {
        // For improved performance ...
        ConfigLog()("pty_buffer_size too small. This cann severily degrade performance. Forcing 256 KB as "
                    "minimum acceptable setting.");
        _config.ptyBufferObjectSize = 1024 * 256;
    }

    tryLoadValue(usedKeys, doc, "reflow_on_resize", _config.reflowOnResize, logger);

    // TODO: If there is only one profile, prefill default_profile with that name.
    // TODO: If there are more than one profile, prefill with the top-most one.
    tryLoadValue(usedKeys, doc, "default_profile", _config.defaultProfileName, logger);

    if (auto profiles = doc["profiles"])
    {
        auto const parentPath = "profiles"s;

        usedKeys.emplace("profiles");
        usedKeys.emplace(fmt::format("{}.{}", parentPath, _config.defaultProfileName));
        _config.profiles[_config.defaultProfileName] =
            loadTerminalProfile(usedKeys,
                                profiles[_config.defaultProfileName],
                                parentPath,
                                _config.defaultProfileName,
                                _config.colorschemes);

        if (!_config.defaultProfileName.empty() && _config.profile(_config.defaultProfileName) == nullptr)
        {
            errorlog()("default_profile \"{}\" not found in profiles list.",
                       escape(_config.defaultProfileName));
        }
        auto dummy = logstore::Category("dymmy", "empty logger", logstore::Category::State::Disabled);

        for (auto i = profiles.begin(); i != profiles.end(); ++i)
        {
            auto const& name = i->first.as<string>();
            if (name == _config.defaultProfileName)
                continue;
            auto const profile = i->second;
            usedKeys.emplace(fmt::format("{}.{}", parentPath, name));
            _config.profiles[name] = _config.profiles[_config.defaultProfileName];
            updateTerminalProfile(
                _config.profiles[name], usedKeys, profile, parentPath, name, _config.colorschemes, dummy());
        }
    }

    if (auto mapping = doc["input_mapping"]; mapping)
    {
        usedKeys.emplace("input_mapping");
        if (mapping.IsSequence())
            for (size_t i = 0; i < mapping.size(); ++i)
            {
                auto prefix = fmt::format("{}.{}", "input_mapping", i);
                parseInputMapping(usedKeys, prefix, _config, mapping[i]);
            }
    }

    checkForSuperfluousKeys(doc, usedKeys);
}

optional<std::string> readConfigFile(std::string const& _filename)
{
    for (FileSystem::path const& prefix: configHomes("contour"))
        if (auto text = readFile(prefix / _filename); text.has_value())
            return text;

    return nullopt;
}

} // namespace contour::config
