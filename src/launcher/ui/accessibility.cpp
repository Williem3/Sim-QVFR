// UI Automation (screen readers such as Narrator, docs/ui/OVERHAUL_PLAN.md §6 decision 10). The window draws itself, so
// it has no child windows to read: this provider makes what the last frame drew into elements under the window, in
// drawn order (reading order):
// - Each control (a hit, app.hits). Its name: its own (a button's label, a segment's item) with the row it sits in
//   ("Up: Ctrl+Alt+Up"), else the row's label (a slider, a pop-up), else its tip. Its help: a switch's line, its tip,
//   or the setting's help (help.cpp). Its kind and what it does: a slider (range value), a segment, tab or game
//   (selection item), a switch (toggle, its state as drawn), a pop-up (its value), a button (invoke).
// - Text the screen reader reads (reader_text: card titles as headings, rows' values, notes, chips) and text with a
//   tip alone (the session chart's summary).
// - While a pop-up menu is open, its items alone (menu items; the one under the arrows has the focus).
// Actions are posted to the window and done as a click or the arrow keys would (kAccessActivateMessage,
// kAccessValueMessage, kAccessMenuMessage). Calls arrive on the window's thread (COM threading: the window's thread is
// an STA). After each paint a focus change raises the focus event, and the focused control's own change (a switch
// toggled, a slider moved, a choice made) its property event. A sheet or a panel clears what is under it, so only it
// is read while it is open.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>

#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace {

// A pop-up menu's item (option: its index in app.popup.items).
constexpr Ctl kMenuItem = static_cast<Ctl>(-2);

// UIA's heading levels (UIA_HeadingLevelPropertyId and HeadingLevel1.., newer SDKs): by value, for any SDK.
constexpr PROPERTYID kHeadingLevelProperty = 30173;
constexpr long kHeadingLevelNone = 80050;

// An element: its control and which one of several (text, kNone or kText: its place among those).
struct Key {
    Ctl id = kNone;
    int option = -1;
    bool operator==(const Key&) const = default;
};

bool counted(Ctl id) { return id == kNone || id == kText; }

bool listed(const Hit& h) {
    if (h.r.w <= 0 || h.r.h <= 0 || h.id == kHelpIcon)
        return false;
    if (h.id == kNone)
        return !h.tip.empty();
    if (h.id == kText)
        return !h.name.empty();
    return true;
}

// The listed hits with their keys, in drawn order.
template <typename F>
void each_hit(F&& f) {
    int none = 0, text = 0;
    for (const Hit& h : app.hits) {
        if (!listed(h))
            continue;
        const int option = h.id == kNone ? none++ : h.id == kText ? text++ : h.option;
        if (!f(h, Key{h.id, option}))
            return;
    }
}

bool menu_open() { return app.popup.open && !app.popup.items.empty(); }

PopupRow menu_kind(size_t i) { return i < app.popup.kinds.size() ? app.popup.kinds[i] : PopupRow::Item; }

// The elements, each once.
std::vector<Key> element_keys() {
    std::vector<Key> keys;
    if (menu_open()) {
        for (size_t i = 0; i < app.popup.items.size(); ++i)
            if (menu_kind(i) != PopupRow::Separator)
                keys.push_back({kMenuItem, static_cast<int>(i)});
        return keys;
    }
    // Reading order: the rail (the games), the top bar (the tabs, Launch), then the page; a panel or a sheet alone.
    std::vector<Key> region[3];
    const bool modal = app.sheet.open || app.panel != PanelKind::None;
    each_hit([&region, modal](const Hit& h, const Key& k) {
        const int r = modal ? 2 : h.r.x < rail_width() ? 0 : h.r.bottom() <= t::kTopbarHeight + 1 ? 1 : 2;
        if (std::find(region[r].begin(), region[r].end(), k) == region[r].end())
            region[r].push_back(k);
        return true;
    });
    for (const std::vector<Key>& part : region)
        keys.insert(keys.end(), part.begin(), part.end());
    return keys;
}

const Hit* find_hit(const Key& k) {
    const Hit* found = nullptr;
    each_hit([&](const Hit& h, const Key& key) {
        if (key == k)
            found = &h;
        return found == nullptr;
    });
    return found;
}

// The label of the row the hit sits in: the smallest drawn label's rect holding its centre (shown rows only).
std::wstring row_label(const Hit& h) {
    const float cx = h.r.x + h.r.w / 2, cy = h.r.y + h.r.h / 2;
    const DrawnLabel* best = nullptr;
    for (const DrawnLabel& d : app.drawnLabels)
        if (d.hiddenBy.empty() && d.more.empty() && d.r.contains(cx, cy) && (!best || d.r.w * d.r.h < best->r.w * best->r.h))
            best = &d;
    return best ? best->label : L"";
}

// The pop-ups (input.cpp opens these with open_popup): a value and a menu of the others.
bool popup_ctl(Ctl id) {
    switch (id) {
    case kMethod:
    case kCheekyOuterPreset:
    case kCheekyFocusPreset:
    case kCheekyNrPreset:
    case kCheekyNrPasses:
    case kFgRelease:
    case kDlssFocusMode:
    case kDlssSharpening:
    case kNrStyle:
    case kNrPreset:
    case kDiagKind: return true;
    default: return false;
    }
}

std::wstring popup_value(Ctl id) {
    std::vector<std::wstring> items;
    const int selected = choice_items(id, items);
    return selected >= 0 && selected < static_cast<int>(items.size()) ? items[static_cast<size_t>(selected)] : L"";
}

std::wstring own_name(const Hit& h) {
    switch (h.id) {
    case kText: return h.name;
    case kSearch: return L"Find a setting (Ctrl+K)";
    case kProfileMenu: return L"Profile menu";
    case kThemeToggle: return L"Theme";
    case kLaunch: return L"Launch " + game_name();
    case kVersion: return L"About Sim-QVFR";
    case kSupport: return L"Support Sim-QVFR on Ko-fi";
    case kPanelClose: return L"Close";
    case kCardMenu:
        return h.option >= 0 && h.option < static_cast<int>(app.cardTitles.size())
                   ? app.cardTitles[static_cast<size_t>(h.option)] + L" card menu"
                   : L"Card menu";
    case kSheetButton:
        return h.option >= 0 && h.option < static_cast<int>(app.sheet.buttons.size()) ? app.sheet.buttons[static_cast<size_t>(h.option)]
                                                                                     : L"";
    case kSheetField: return app.sheet.fieldLabel;
    case kDiagDescription: return L"What happened?";
    default: break;
    }
    const std::wstring row = row_label(h);
    if (!h.name.empty())
        return row.empty() || row == h.name || h.id == kTopTab || h.id == kGameTab ? h.name : row + L": " + h.name;
    if (!row.empty())
        return row;
    return h.tip;
}

long control_type(const Hit& h) {
    if (counted(h.id))
        return UIA_TextControlTypeId;
    if (h.id == kTopTab)
        return UIA_TabItemControlTypeId;
    if (h.id == kGameTab)
        return UIA_ListItemControlTypeId;
    if (h.id == kSheetField || h.id == kSearchField || h.id == kDiagDescription)
        return UIA_EditControlTypeId;
    if (slider_value(h.id))
        return UIA_SliderControlTypeId;
    if (h.id == kTopTab || h.id == kGameTab || h.id == kColor || segmented_ctl(h.id))
        return UIA_RadioButtonControlTypeId;
    if (popup_ctl(h.id))
        return UIA_ComboBoxControlTypeId;
    const bool swatch = h.id == kColorGame || h.id == kColorPreset || h.id == kColorReset || h.id == kColorCustom;
    if (h.state >= 0 && h.id != kMore && !swatch)
        return UIA_CheckBoxControlTypeId;
    return UIA_ButtonControlTypeId;
}

// What an element is now: from its hit, or from the open pop-up menu.
struct Info {
    Rect r;
    std::wstring name, help;
    long type = UIA_TextControlTypeId;
    int state = -1;    // a switch: 0 off, 1 on; a choice: 1 the chosen one
    int heading = -1;  // text: 1-3
};

bool info_of(const Key& k, Info& out) {
    if (k.id == kMenuItem) {
        if (!menu_open() || k.option < 0 || k.option >= static_cast<int>(app.popup.items.size()))
            return false;
        const Rect menu = popup_menu_rect();
        float y = menu.y + 6;
        for (int i = 0; i < k.option; ++i)
            y += popup_row_height(static_cast<size_t>(i));
        out.r = {menu.x + 6, y, menu.w - 12, popup_row_height(static_cast<size_t>(k.option))};
        out.name = app.popup.items[static_cast<size_t>(k.option)];
        const bool item = menu_kind(static_cast<size_t>(k.option)) == PopupRow::Item;
        out.type = item ? UIA_MenuItemControlTypeId : UIA_TextControlTypeId;
        out.state = item && k.option == app.popup.selected ? 1 : -1;
        if (out.state == 1)
            out.name += L", chosen";
        return true;
    }
    const Hit* h = find_hit(k);
    if (!h || menu_open())
        return false;
    out.r = h->r;
    out.type = control_type(*h);
    out.state = h->state;
    out.heading = h->id == kText ? h->state : -1;
    out.name = own_name(*h);
    const bool swatch = h->id == kColorReset || h->id == kColorPreset || h->id == kColorCustom;
    if (swatch && h->state == 1)
        out.name += L", chosen";
    // Its help: a switch's line under its label, then its tip or the setting's own help.
    std::wstring help = h->desc;
    std::wstring more = h->tip;
    if (h->id == kSheetButton)
        more = app.sheet.title + (app.sheet.title.empty() || std::wstring(L"?.!").find(app.sheet.title.back()) != std::wstring::npos
                                      ? L" "
                                      : L". ") +
               app.sheet.text;
    else if (more.empty() && !counted(h->id))
        if (const wchar_t* text = help_for(row_label(*h)))
            more = text;
    if (!more.empty() && more != out.name)
        help += (help.empty() ? L"" : L" ") + more;
    out.help = help;
    return true;
}

bool has_focus(const Key& k) {
    if (k.id == kMenuItem)
        return menu_open() && app.popup.hover == k.option;
    if (menu_open())
        return false;
    return app.focus == k.id && !counted(k.id) && (k.option < 0 || app.focusOption == k.option);
}

UiaRect screen_rect(const Rect& r) {
    POINT origin{0, 0};
    ClientToScreen(app.hwnd, &origin);
    const double s = app.canvas.dpi_scale();
    return {origin.x + r.x * s, origin.y + r.y * s, r.w * s, r.h * s};
}

void set_bool(VARIANT* v, bool b) {
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

void set_string(VARIANT* v, const std::wstring& s) {
    v->vt = VT_BSTR;
    v->bstrVal = SysAllocString(s.c_str());
}

class Root;
Root* g_root = nullptr;
IRawElementProviderFragmentRoot* root_interface();
IRawElementProviderFragment* root_fragment();

// One element, by its key; each call looks it up in the last frame (gone: not available).
class Element final : public IRawElementProviderSimple,
                      public IRawElementProviderFragment,
                      public IInvokeProvider,
                      public IToggleProvider,
                      public ISelectionItemProvider,
                      public IRangeValueProvider,
                      public IValueProvider {
public:
    explicit Element(Key key) : key_(key) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&refs_);
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
            *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment))
            *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IInvokeProvider))
            *out = static_cast<IInvokeProvider*>(this);
        else if (riid == __uuidof(IToggleProvider))
            *out = static_cast<IToggleProvider*>(this);
        else if (riid == __uuidof(ISelectionItemProvider))
            *out = static_cast<ISelectionItemProvider*>(this);
        else if (riid == __uuidof(IRangeValueProvider))
            *out = static_cast<IRangeValueProvider*>(this);
        else if (riid == __uuidof(IValueProvider))
            *out = static_cast<IValueProvider*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* ret) override {
        *ret = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** ret) override {
        *ret = nullptr;
        Info info;
        if (!info_of(key_, info))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const long type = info.type;
        IUnknown* p = nullptr;
        if (pattern == UIA_InvokePatternId &&
            (type == UIA_ButtonControlTypeId || type == UIA_ComboBoxControlTypeId || type == UIA_MenuItemControlTypeId))
            p = static_cast<IInvokeProvider*>(this);
        else if (pattern == UIA_TogglePatternId && type == UIA_CheckBoxControlTypeId)
            p = static_cast<IToggleProvider*>(this);
        else if (pattern == UIA_SelectionItemPatternId &&
                 (type == UIA_RadioButtonControlTypeId || type == UIA_TabItemControlTypeId || type == UIA_ListItemControlTypeId))
            p = static_cast<ISelectionItemProvider*>(this);
        else if (pattern == UIA_RangeValuePatternId && type == UIA_SliderControlTypeId)
            p = static_cast<IRangeValueProvider*>(this);
        else if (pattern == UIA_ValuePatternId && type == UIA_ComboBoxControlTypeId)
            p = static_cast<IValueProvider*>(this);
        if (p) {
            p->AddRef();
            *ret = p;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* ret) override {
        ret->vt = VT_EMPTY;
        Info info;
        if (!info_of(key_, info))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const bool text = info.type == UIA_TextControlTypeId;
        switch (property) {
        case UIA_NamePropertyId: set_string(ret, info.name); break;
        case UIA_ControlTypePropertyId:
            ret->vt = VT_I4;
            ret->lVal = info.type;
            break;
        case UIA_HelpTextPropertyId:
            if (!info.help.empty())
                set_string(ret, info.help);
            break;
        case UIA_AutomationIdPropertyId:
            set_string(ret, L"ctl" + std::to_wstring(static_cast<int>(key_.id)) + L"_" + std::to_wstring(key_.option));
            break;
        case UIA_IsKeyboardFocusablePropertyId: set_bool(ret, !text); break;
        case UIA_HasKeyboardFocusPropertyId: set_bool(ret, has_focus(key_) && GetForegroundWindow() == app.hwnd); break;
        case UIA_IsEnabledPropertyId:
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: set_bool(ret, true); break;
        case UIA_IsOffscreenPropertyId: {
            const Rect& a = app.panel != PanelKind::None ? app.panelBody : app.scrollArea;
            const bool inPage = info.r.bottom() > a.y && info.r.y < a.bottom();
            const bool chrome = info.r.y < t::kTopbarHeight || info.r.x < rail_width();  // the top bar and the rail
            set_bool(ret, !(inPage || chrome || app.sheet.open || key_.id == kMenuItem));
            break;
        }
        case kHeadingLevelProperty:
            if (info.heading >= 1) {
                ret->vt = VT_I4;
                ret->lVal = kHeadingLevelNone + info.heading;
            }
            break;
        case UIA_ProviderDescriptionPropertyId: set_string(ret, L"Sim-QVFR launcher"); break;
        default: break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
        *ret = nullptr;
        return S_OK;
    }

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        if (direction == NavigateDirection_Parent) {
            *ret = root_fragment();
            return S_OK;
        }
        if (direction != NavigateDirection_NextSibling && direction != NavigateDirection_PreviousSibling)
            return S_OK;  // no children
        const std::vector<Key> keys = element_keys();
        const auto it = std::find(keys.begin(), keys.end(), key_);
        if (it == keys.end())
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (direction == NavigateDirection_NextSibling && it + 1 != keys.end())
            *ret = new Element(*(it + 1));
        else if (direction == NavigateDirection_PreviousSibling && it != keys.begin())
            *ret = new Element(*(it - 1));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** ret) override {
        int ids[] = {UiaAppendRuntimeId, static_cast<int>(key_.id) + 1000, key_.option + 2};
        *ret = SafeArrayCreateVector(VT_I4, 0, 3);
        if (!*ret)
            return E_OUTOFMEMORY;
        for (LONG i = 0; i < 3; ++i)
            SafeArrayPutElement(*ret, &i, &ids[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* ret) override {
        Info info;
        if (!info_of(key_, info))
            return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = screen_rect(info.r);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** ret) override {
        *ret = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        Info info;
        if (!info_of(key_, info) || info.type == UIA_TextControlTypeId)
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (key_.id == kMenuItem)
            app.popup.hover = key_.option;
        else {
            app.focus = key_.id;
            app.focusOption = key_.option;
            app.keyboard = true;
        }
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override {
        *ret = root_interface();
        return *ret ? S_OK : E_FAIL;
    }

    // IInvokeProvider, IToggleProvider: pressed as a click would.
    HRESULT STDMETHODCALLTYPE Invoke() override { return press(); }
    HRESULT STDMETHODCALLTYPE Toggle() override { return press(); }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* ret) override {
        Info info;
        if (!info_of(key_, info))
            return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = info.state == 1 ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // ISelectionItemProvider: a segment, a tab, a game.
    HRESULT STDMETHODCALLTYPE Select() override { return press(); }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return press(); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* ret) override {
        Info info;
        if (!info_of(key_, info))
            return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = info.state == 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** ret) override {
        *ret = nullptr;
        return S_OK;
    }

    // IRangeValueProvider: a slider, set as the arrow keys would.
    HRESULT STDMETHODCALLTYPE SetValue(double value) override {
        if (!find_hit(key_) || !slider_value(key_.id))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const Range r = slider_range(key_.id);
        if (value < r.min || value > r.max)
            return E_INVALIDARG;
        PostMessageW(app.hwnd, kAccessValueMessage, static_cast<WPARAM>(key_.id), static_cast<LPARAM>(std::lround(value)));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Value(double* ret) override {
        const int* v = slider_value(key_.id);
        if (!v)
            return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = *v;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* ret) override {
        *ret = slider_value(key_.id) ? FALSE : TRUE;  // a slider is set here; a pop-up's value through its menu
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Maximum(double* ret) override {
        *ret = slider_range(key_.id).max;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Minimum(double* ret) override {
        *ret = slider_range(key_.id).min;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* ret) override {
        *ret = 10;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* ret) override {
        *ret = 1;
        return S_OK;
    }

    // IValueProvider: a pop-up's chosen item.
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR) override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* ret) override {
        *ret = SysAllocString(popup_value(key_.id).c_str());
        return S_OK;
    }

private:
    HRESULT press() {
        Info info;
        if (!info_of(key_, info) || info.type == UIA_TextControlTypeId)
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (key_.id == kMenuItem)
            PostMessageW(app.hwnd, kAccessMenuMessage, static_cast<WPARAM>(key_.option), 0);
        else
            PostMessageW(app.hwnd, kAccessActivateMessage, static_cast<WPARAM>(key_.id), static_cast<LPARAM>(key_.option));
        return S_OK;
    }

    Key key_;
    LONG refs_ = 1;
};

// The window's fragment root: the elements are its children; the window's own properties come from its HWND.
class Root final : public IRawElementProviderSimple, public IRawElementProviderFragment, public IRawElementProviderFragmentRoot {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&refs_);
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
            *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment))
            *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot))
            *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* ret) override {
        *ret = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** ret) override {
        *ret = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* ret) override {
        ret->vt = VT_EMPTY;
        if (property == UIA_NamePropertyId)
            set_string(ret, L"Sim-QVFR Launcher");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
        return UiaHostProviderFromHwnd(app.hwnd, ret);
    }

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        if (direction == NavigateDirection_FirstChild || direction == NavigateDirection_LastChild) {
            const std::vector<Key> keys = element_keys();
            if (!keys.empty())
                *ret = new Element(direction == NavigateDirection_FirstChild ? keys.front() : keys.back());
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** ret) override {
        *ret = nullptr;  // the HWND's
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* ret) override {
        *ret = {};  // the HWND's
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** ret) override {
        *ret = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        ::SetFocus(app.hwnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override {
        AddRef();
        *ret = this;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        POINT p{static_cast<LONG>(x), static_cast<LONG>(y)};
        ScreenToClient(app.hwnd, &p);
        const double s = app.canvas.dpi_scale();
        const float px = static_cast<float>(p.x / s), py = static_cast<float>(p.y / s);
        // The topmost: the menu's item, else the last drawn element there (a control over the text it sits on).
        std::vector<Key> keys;
        if (menu_open())
            keys = element_keys();
        else
            each_hit([&keys](const Hit&, const Key& k) {
                keys.push_back(k);
                return true;
            });
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
            Info info;
            if (info_of(*it, info) && info.r.contains(px, py) && (info.type != UIA_TextControlTypeId || !*ret)) {
                if (*ret)
                    (*ret)->Release();
                *ret = new Element(*it);
                if (info.type != UIA_TextControlTypeId)
                    break;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        for (const Key& k : element_keys())
            if (has_focus(k)) {
                *ret = new Element(k);
                break;
            }
        return S_OK;
    }

private:
    LONG refs_ = 1;
};

IRawElementProviderFragmentRoot* root_interface() {
    if (!g_root)
        return nullptr;
    g_root->AddRef();
    return g_root;
}

IRawElementProviderFragment* root_fragment() {
    if (!g_root)
        return nullptr;
    g_root->AddRef();
    return g_root;
}

// What the last paint showed of the focused element, to tell its changes.
struct Seen {
    Key key;
    bool known = false;
    int state = -1;
    int value = 0;
    std::wstring valueText;
};

Seen g_seen;

}  // namespace

bool accessibility_object(HWND hwnd, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    if (static_cast<long>(lParam) != static_cast<long>(UiaRootObjectId))
        return false;
    if (!g_root)
        g_root = new Root();
    result = UiaReturnRawElementProvider(hwnd, wParam, lParam, static_cast<IRawElementProviderSimple*>(g_root));
    return true;
}

void accessibility_after_paint() {
    if (!g_root || !UiaClientsAreListening())
        return;
    Key focus{};
    bool any = false;
    for (const Key& k : element_keys())
        if (has_focus(k)) {
            focus = k;
            any = true;
            break;
        }
    Info info;
    if (!any || !info_of(focus, info)) {
        g_seen = {};
        return;
    }
    const int* value = focus.id != kMenuItem ? slider_value(focus.id) : nullptr;
    const std::wstring text = focus.id != kMenuItem && popup_ctl(focus.id) ? popup_value(focus.id) : L"";
    Element* el = new Element(focus);
    if (!g_seen.known || !(g_seen.key == focus)) {
        UiaRaiseAutomationEvent(el, UIA_AutomationFocusChangedEventId);
    } else {
        if (info.state != g_seen.state && info.state >= 0) {
            VARIANT before{}, after{};
            if (info.type == UIA_CheckBoxControlTypeId) {
                before.vt = after.vt = VT_I4;
                before.lVal = g_seen.state == 1 ? ToggleState_On : ToggleState_Off;
                after.lVal = info.state == 1 ? ToggleState_On : ToggleState_Off;
                UiaRaiseAutomationPropertyChangedEvent(el, UIA_ToggleToggleStatePropertyId, before, after);
            } else {
                set_bool(&before, g_seen.state == 1);
                set_bool(&after, info.state == 1);
                UiaRaiseAutomationPropertyChangedEvent(el, UIA_SelectionItemIsSelectedPropertyId, before, after);
                if (info.state == 1)
                    UiaRaiseAutomationEvent(el, UIA_SelectionItem_ElementSelectedEventId);
            }
        }
        if (value && *value != g_seen.value) {
            VARIANT before{}, after{};
            before.vt = after.vt = VT_R8;
            before.dblVal = g_seen.value;
            after.dblVal = *value;
            UiaRaiseAutomationPropertyChangedEvent(el, UIA_RangeValueValuePropertyId, before, after);
        }
        if (text != g_seen.valueText) {
            VARIANT before{}, after{};
            set_string(&before, g_seen.valueText);
            set_string(&after, text);
            UiaRaiseAutomationPropertyChangedEvent(el, UIA_ValueValuePropertyId, before, after);
            VariantClear(&before);
            VariantClear(&after);
        }
    }
    el->Release();
    g_seen = {focus, true, info.state, value ? *value : 0, text};
}

void accessibility_close() {
    if (!g_root)
        return;
    UiaReturnRawElementProvider(app.hwnd, 0, 0, nullptr);  // the window goes: clients let go of its provider
    UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(g_root));
    g_root->Release();
    g_root = nullptr;
}

}  // namespace simqvfr::launcher::ui::win
