
/*
  __        ___.         .__          __
_/  |______ \_ |__  __ __|  | _____ _/  |_  ____
\   __\__  \ | __ \|  |  \  | \__  \\   __\/ __ \
 |  |  / __ \| \_\ \  |  /  |__/ __ \|  | \  ___/
 |__| (____  /___  /____/|____(____  /__|  \___  >
           \/    \/                \/          \/
Table Maker for Modern C++
https://github.com/p-ranav/tabulate

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2019 Pranav Srinivas Kumar <pranav.srinivas.kumar@gmail.com>.

Permission is hereby  granted, free of charge, to any  person obtaining a copy
of this software and associated  documentation files (the "Software"), to deal
in the Software  without restriction, including without  limitation the rights
to  use, copy,  modify, merge,  publish, distribute,  sublicense, and/or  sell
copies  of  the Software,  and  to  permit persons  to  whom  the Software  is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS OR
IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF  MERCHANTABILITY,
FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN NO EVENT  SHALL THE
AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY  CLAIM,  DAMAGES OR  OTHER
LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

#include <clocale>
#include <locale>

#include <cstdlib>
#include <tabulate/termcolor.hpp>
#include <wchar.h>

namespace tabulate {

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
// OSv slim libc++ is built without wide characters (no wcswidth/mbstowcs/wide
// locale facets), so the upstream wcswidth() path is unavailable. All tabulate
// output here is ASCII, where display width equals the UTF-8 codepoint count;
// approximate it by counting non-continuation bytes (matches the multibyte
// fallback in get_sequence_length below).
inline int get_wcswidth(const std::string &string, const std::string &locale,
                        size_t max_column_width) {
  (void)locale;
  (void)max_column_width;
  return static_cast<int>(
      string.length() - std::count_if(string.begin(), string.end(),
                                      [](char c) -> bool { return (c & 0xC0) == 0x80; }));
}
#endif

inline size_t get_sequence_length(const std::string &text, const std::string &locale,
                                  bool is_multi_byte_character_support_enabled) {
  if (!is_multi_byte_character_support_enabled)
    return text.length();

#if defined(_WIN32) || defined(_WIN64)
  (void)locale; // unused parameter
  return (text.length() - std::count_if(text.begin(), text.end(),
                                        [](char c) -> bool { return (c & 0xC0) == 0x80; }));
#elif defined(__unix__) || defined(__unix) || defined(__APPLE__)
  auto result = get_wcswidth(text, locale, text.size());
  if (result >= 0)
    return result;
  else
    return (text.length() - std::count_if(text.begin(), text.end(),
                                          [](char c) -> bool { return (c & 0xC0) == 0x80; }));
#endif
}

} // namespace tabulate
