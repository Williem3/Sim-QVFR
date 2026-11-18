// I18N(L"...") marks English text for tools/lang_extract.py where it can't be translated yet (a constexpr table shared
// with the layer, a log or the ini): the code passes it through lang::tr() (lang.h) where it shows. Expands to the
// literal itself; nothing else, so any header can include it.
#pragma once

#define I18N(text) text
