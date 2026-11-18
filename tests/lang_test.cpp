// Offline check of the language files (src/lang.h): the format, what a file may not do (placeholders that differ,
// oversized or invalid text), lookups and placeholder filling, choosing a file, and every lang/*.ini the release ships.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstdint>
#include <string>

#include "lang.h"

namespace fs = std::filesystem;
using namespace simqvfr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

std::wstring get(const lang::Table& t, const wchar_t* english) {
    const auto it = t.text.find(std::wstring_view(english));
    return it == t.text.end() ? L"" : it->second;
}

void format() {
    lang::Table t;
    const bool ok = lang::parse("\xEF\xBB\xBF; a comment\r\n"
                                "[language]\r\ncode = de\r\nname = Deutsch\r\n"
                                "[strings]\r\n"
                                "en = Check now\r\nde = Jetzt pr\xC3\xBC" "fen\r\n"
                                "en = Launch {game}\r\nde = {game} starten\r\n"
                                "en = \"  \xC2\xB7  changed since\"\r\nde = \"  \xC2\xB7  seitdem ge\xC3\xA4ndert\"\r\n"
                                "en = Two\\nlines\r\nde = Zwei\\nZeilen\r\n"
                                "en = licenses\\\\x.txt\r\nde = Lizenzen\\\\x.txt\r\n"
                                "en = Not translated yet\r\nde =\r\n",
                                t);
    check(ok, "a language file parses");
    check(t.name == L"Deutsch", "[language] name");
    check(get(t, L"Check now") == L"Jetzt prüfen", "a plain line, UTF-8 after a BOM");
    check(get(t, L"Launch {game}") == L"{game} starten", "a placeholder may move");
    check(get(t, L"  ·  changed since") == L"  ·  seitdem geändert", "quotes keep spaces at the ends");
    check(get(t, L"Two\nlines") == L"Zwei\nZeilen", "\\n is a line break");
    check(get(t, L"licenses\\x.txt") == L"Lizenzen\\x.txt", "\\\\ is a backslash");
    check(t.text.find(std::wstring_view(L"Not translated yet")) == t.text.end(), "an empty translation is left out");
    check(t.refused == 0, "nothing refused in a good file");
    lang::Table none;
    check(!lang::parse("[language]\nname = x\n", none), "no [strings]: not a language file");
}

void refusals() {
    lang::Table t;
    lang::parse("[strings]\n"
                "en = Launch {game}\nde = starten\n"                    // a placeholder dropped
                "en = Version {version}\nde = Version {game}\n"         // a different one
                "en = Hello\nde = Hallo {secret}\n"                     // one added
                "en = A {x} and {x}\nde = Ein {x}\n"                    // one use dropped
                "en = Broken\nde = \"unterminated\n"                    // an unterminated quote
                "en = Escape\nde = bad \\q escape\n"                    // an unknown escape
                "en = Bytes\nde = \xFF\xFE invalid\n"                   // invalid UTF-8: its line and the pair skipped
                "de = Orphan without English\n"
                "en = Kept\nde = Behalten\n",
                t);
    check(t.text.size() == 1 && get(t, L"Kept") == L"Behalten", "only the good translation is taken");
    check(t.refused == 7, "each bad line counted");
    std::string longValue = "[strings]\nen = Long\nde = " + std::string(lang::kMaxValueChars + 1, 'x') + "\n";
    lang::Table l;
    lang::parse(longValue, l);
    check(l.text.empty() && l.refused == 1, "an over-long translation is refused");
    lang::Table big;
    check(!lang::parse(std::string(lang::kMaxFileBytes + 1, ' '), big), "an over-large file is refused");
}

void lookups() {
    check(lang::fill(L"{a} and {b}", {{L"a", L"{b}"}, {L"b", L"x"}}) == L"{b} and x", "a value isn't expanded again");
    check(lang::fill(L"{{literal}} {missing}", {}) == L"{literal} {missing}", "braces and unknown names");
    lang::use(nullptr);
    const wchar_t* english = L"Check now";
    check(lang::tr(english) == english, "English without a table: the same pointer");
    auto t = std::make_unique<lang::Table>();
    t->text[L"Check now"] = L"Jetzt prüfen";
    t->text[L"Launch {game}"] = L"{game} starten";
    const uint32_t before = lang::generation();
    const lang::Table* first = lang::use(std::move(t));
    check(lang::generation() == before + 1, "a change of language moves generation() (the menu redraws)");
    const wchar_t* german = lang::tr(L"Check now");
    check(std::wstring(german) == L"Jetzt prüfen", "tr finds the translation");
    check(std::wstring(lang::tr(L"Unknown")) == L"Unknown", "a missing line shows in English");
    check(lang::trf(L"Launch {game}", {{L"game", L"LMU"}}) == L"LMU starten", "trf translates and fills");
    auto other = std::make_unique<lang::Table>();
    lang::use(std::move(other));
    check(std::wstring(german) == L"Jetzt prüfen" && lang::current() != first, "an earlier table outlives a change");
    lang::use(nullptr);
}

void choosing() {
    const fs::path dir = fs::temp_directory_path() / L"sim-qvfr-lang-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto write = [&](const wchar_t* name, const char* text) { std::ofstream(dir / name, std::ios::binary) << text; };
    write(L"de.ini", "[language]\nname = Deutsch\n[strings]\nen = Check now\nde = Jetzt pr\xC3\xBC" "fen\n");
    write(L"pt-BR.ini", "[language]\nname = Portugu\xC3\xAAs (Brasil)\n[strings]\n");
    write(L"_template.ini", "[strings]\n");
    write(L"en.ini", "[strings]\n");
    write(L"bad name.ini", "[strings]\n");
    write(L"notes.ini", "[general]\n");
    const auto found = lang::available(dir);
    check(found.size() == 2 && found[0].code == L"de" && found[1].code == L"pt-BR", "the template, en, bad names and non-files skipped");
    check(lang::resolve(L"de-AT", found) == L"de", "a region falls back to its language");
    check(lang::resolve(L"pt", found) == L"pt-BR", "a language finds a region's file");
    check(lang::resolve(L"en", found).empty() && lang::resolve(L"fr", found).empty(), "English, or no file: English");
    check(lang::select(dir, L"de") == L"de" && std::wstring(lang::tr(L"Check now")) == L"Jetzt prüfen", "select loads it");
    check(lang::select(dir, L"en").empty() && std::wstring(lang::tr(L"Check now")) == L"Check now", "select English");
    check(lang::folder_near(dir).filename() == L"lang", "the lang folder beside or above a module");
    fs::remove_all(dir, ec);
}

// Every file the release ships reads clean: none refused, the template among them.
void shipped() {
    int files = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(SIM_QVFR_LANG_DIR), ec)) {
        if (entry.path().extension() != L".ini")
            continue;
        lang::Table t;
        const bool ok = lang::load_file(entry.path(), t);
        const std::string name = entry.path().filename().string();
        check(ok, ("lang/" + name + " is a language file").c_str());
        check(t.refused == 0, ("lang/" + name + ": no line refused (placeholders, quotes, escapes)").c_str());
        std::printf("     lang/%s: %zu translated\n", name.c_str(), t.text.size());
        ++files;
    }
    check(files > 0, "lang/ has files");
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    format();
    refusals();
    lookups();
    choosing();
    shipped();
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
