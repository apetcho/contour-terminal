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
#include <vtbackend/cell/CompactCell.h>

namespace terminal
{

std::u32string CompactCell::codepoints() const
{
    std::u32string s;
    if (_codepoint)
    {
        s += _codepoint;
        if (_extra)
        {
            for (char32_t const cp: _extra->codepoints)
            {
                s += cp;
            }
        }
    }
    return s;
}

std::string CompactCell::toUtf8() const
{
    if (!_codepoint)
        return {};

    std::string text;
    text += unicode::convert_to<char>(_codepoint);
    if (_extra)
        for (char32_t const cp: _extra->codepoints)
            text += unicode::convert_to<char>(cp);
    return text;
}

} // namespace terminal
