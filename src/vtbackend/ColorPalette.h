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

#include <vtbackend/Color.h>
#include <vtbackend/Image.h>

#include <crispy/StrongHash.h>
#include <crispy/stdfs.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>

namespace terminal
{

struct ImageData
{
    terminal::ImageFormat format;
    int rowAlignment = 1;
    ImageSize size;
    std::vector<uint8_t> pixels;

    crispy::StrongHash hash;

    void updateHash() noexcept;
};

using ImageDataPtr = std::shared_ptr<ImageData const>;

struct BackgroundImage
{
    using Location = std::variant<FileSystem::path, ImageDataPtr>;

    Location location;
    crispy::StrongHash hash;

    // image configuration
    float opacity = 1.0; // normalized value
    bool blur = false;
};

struct ColorPalette
{
    using Palette = std::array<RGBColor, 256 + 8>;

    /// Indicates whether or not bright colors are being allowed
    /// for indexed colors between 0..7 and mode set to ColorMode::Bright.
    ///
    /// This value is used by draw_bold_text_with_bright_colors in profile configuration.
    ///
    /// If disabled, normal color will be used instead.
    ///
    /// TODO: This should be part of Config's Profile instead of being here. That sounds just wrong.
    /// TODO: And even the naming sounds wrong. Better would be makeIndexedColorsBrightForBoldText or similar.
    bool useBrightColors = false;

    static Palette const defaultColorPalette;

    Palette palette = defaultColorPalette;

    [[nodiscard]] RGBColor normalColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette.at(index);
    }

    [[nodiscard]] RGBColor brightColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette.at(index + 8);
    }

    [[nodiscard]] RGBColor dimColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette[256 + index];
    }

    [[nodiscard]] RGBColor indexedColor(size_t index) const noexcept
    {
        assert(index < 256);
        return palette.at(index);
    }

    RGBColor defaultForeground = 0xD0D0D0_rgb;
    RGBColor defaultBackground = 0x000000_rgb;

    CursorColor cursor;

    RGBColor mouseForeground = 0x800000_rgb;
    RGBColor mouseBackground = 0x808000_rgb;

    struct
    {
        RGBColor normal = 0x0070F0_rgb;
        RGBColor hover = 0xFF0000_rgb;
    } hyperlinkDecoration;

    RGBColorPair inputMethodEditor = { 0xFFFFFF_rgb, 0xFF0000_rgb };

    std::shared_ptr<BackgroundImage const> backgroundImage;

    // clang-format off
    CellRGBColorAndAlphaPair yankHighlight { CellForegroundColor {}, 1.0f, 0xffA500_rgb, 0.5f };

    CellRGBColorAndAlphaPair searchHighlight { CellBackgroundColor {}, 1.0f, CellForegroundColor {}, 1.0f };
    CellRGBColorAndAlphaPair searchHighlightFocused { CellForegroundColor {}, 1.0f, RGBColor{0xFF, 0x30, 0x30}, 0.5f };

    CellRGBColorAndAlphaPair wordHighlight { CellForegroundColor {}, 1.0f, RGBColor{0x30, 0x90, 0x90}, 0.4f };
    CellRGBColorAndAlphaPair wordHighlightCurrent { CellForegroundColor {}, 1.0f, RGBColor{0x90, 0x90, 0x90}, 0.6f };

    CellRGBColorAndAlphaPair selection { CellBackgroundColor {}, 1.0f, CellForegroundColor {}, 1.0f };

    CellRGBColorAndAlphaPair normalModeCursorline = { 0xFFFFFF_rgb, 0.2f, 0x808080_rgb, 0.8f };
    // clang-format on

    RGBColorPair indicatorStatusLine = { 0x000000_rgb, 0x808080_rgb };
    RGBColorPair indicatorStatusLineInactive = { 0x000000_rgb, 0x808080_rgb };
};

enum class ColorTarget
{
    Foreground,
    Background,
};

enum class ColorMode
{
    Dimmed,
    Normal,
    Bright
};

RGBColor apply(ColorPalette const& colorPalette, Color color, ColorTarget target, ColorMode mode) noexcept;

} // namespace terminal

// {{{ fmtlib custom formatter support
template <>
struct fmt::formatter<terminal::ColorMode>: fmt::formatter<std::string_view>
{
    auto format(terminal::ColorMode value, fmt::format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::ColorMode::Normal: name = "Normal"; break;
            case terminal::ColorMode::Dimmed: name = "Dimmed"; break;
            case terminal::ColorMode::Bright: name = "Bright"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::ColorTarget>: fmt::formatter<std::string_view>
{
    auto format(terminal::ColorTarget value, fmt::format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::ColorTarget::Foreground: name = "Foreground"; break;
            case terminal::ColorTarget::Background: name = "Background"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
// }}}
