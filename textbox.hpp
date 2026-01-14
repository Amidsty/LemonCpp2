#include <SFML/Graphics.hpp>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <string>
#include <cctype>
#include <vector>
#include <filesystem>
#include <stack>
#include <set>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <string>
#include <cmath>

namespace sf{typedef uint32_t Uint32;}

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

// UTF-8 helper functions (encode/decode between sf::String (UTF-32) and UTF-8 std::string)
static std::string utf8_encode_char(sf::Uint32 ch) {
    std::string out;
    if (ch <= 0x7F) {
        out.push_back((char)ch);
    } else if (ch <= 0x7FF) {
        out.push_back((char)(0xC0 | ((ch >> 6) & 0x1F)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    } else if (ch <= 0xFFFF) {
        out.push_back((char)(0xE0 | ((ch >> 12) & 0x0F)));
        out.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | ((ch >> 18) & 0x07)));
        out.push_back((char)(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    }
    return out;
}

static std::string utf8_encode_string(const sf::String &s) {
    std::string out;
    for (auto ch: s) out += utf8_encode_char((sf::Uint32)ch);
    return out;
}

static sf::String utf8_decode_string(const std::string &s) {
    sf::String out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        sf::Uint32 code = 0;
        size_t extra = 0;
        if ((c & 0x80) == 0) { code = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { code = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { code = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { code = c & 0x07; extra = 3; }
        else { // invalid byte, treat as replacement
            code = 0xFFFD; extra = 0;
        }
        ++i;
        for (size_t k = 0; k < extra && i < s.size(); ++k, ++i) {
            unsigned char cc = (unsigned char)s[i];
            if ((cc & 0xC0) != 0x80) break;
            code = (code << 6) | (cc & 0x3F);
        }
        out += (wchar_t)code;
    }
    return out;
}

struct Cursor {
    std::size_t row = 0;
    std::size_t col = 0;
};

struct Selection {
    bool active = false;
    Cursor start, end;
};

struct EditAction {
    std::vector<sf::String> before;
    std::vector<sf::String> after;
    Cursor cursorBefore, cursorAfter;
};

std::set<sf::String> highlight_word={
	"asm","and","auto","bool",
	"break","case","catch",
	"char","class","const","constexpr",
	"const_cast","continue",
	"default","delete","do",
	"double","dynamic_cast","else",
	"enum","explicit","export",
	"extern","false","float",
	"for","friend","goto",
	"if","inline","int",
	"long","mutable","namespace",
	"new","nullptr","operator","or",
	"private","protected","public",
	"register","reinterpret_cast","return",
	"short","signed","sizeof",
	"static","static_cast","struct",
	"switch","template","this",
	"throw","true","try",
	"typedef","typeid","typename",
	"union","unsigned","using",
	"virtual","void","volatile",
	"wchar_t","while","xor",
	"and_eq","bitand","bitor","compl","not","not_eq","or_eq","xor_eq"
};
std::set<sf::String> extend_highlight_word={
	"std","string","vector",
	"map","set","stack",
	"cout","cin","cerr","endl",
    "wcout","wcin","wcerr","wendl",
	"printf","scanf","main",
    "__int128","int8_t","int16_t","int32_t","int64_t",
    "uint8_t","uint16_t","uint32_t","uint64_t",
    "size_t","uint32","int32","uint64","int64",
    "string_view","to_string","to_wstring",
    "make_pair","make_tuple","pair","tuple",
    "emplace_back","push_back","insert","find","begin","end",
    "unique_ptr","shared_ptr","weak_ptr","make_unique","make_shared",
    "vector","list","deque","queue","priority_queue",
    "map","unordered_map","set","unordered_set",
    "__float128","float16_t","float32_t","float64_t","float128_t",
    "NULL","basic_string","ifstream","ofstream","fstream",
    "array","initializer_list","move","forward",
    "istringstream","ostringstream","stringstream"
};
std::set<sf::String> symbol={
	"(",")","{","}","[","]",
	";",":",",",".","->","?",
	"+","-","*","/","%","++","--",
	"==","!=","<",">","<=",">=",
	"&&","||","!","=",
	"+=","-=","*=","/=","%=",
	"<<",">>","<<=",">>=",
	"&","|","^","~",
	"&=","|=","^="
};

class TextBox {
private:
    static std::string colorToHex(const sf::Color &c) {
        char buf[8];
        std::sprintf(buf, "%02X%02X%02X", c.r, c.g, c.b);
        return std::string(buf);
    }
    static sf::Color hexToColor(const std::string &s) {
        if (s.size() < 6) return sf::Color::Black;
        unsigned int r=0,g=0,b=0;
        std::sscanf(s.c_str(), "%2X%2X%2X", &r,&g,&b);
        return sf::Color((uint8_t)r,(uint8_t)g,(uint8_t)b);
    }
    void loadColors() {
        try {
            std::ifstream ifs("colors.cfg");
            if (!ifs) return;
            std::string line;
            int idx = 0;
            while (std::getline(ifs, line) && idx < CR_COUNT) {
                // allow comments and blank lines
                if (line.empty() || line[0] == '#') continue;
                // trim
                while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
                if (line.empty()) continue;
                roleColors[idx] = hexToColor(line);
                idx++;
            }
        } catch(...) {}
    }
    void saveColors() {
        try {
            std::ofstream ofs("colors.cfg");
            if (!ofs) return;
            for (int i=0;i<CR_COUNT;i++) {
                ofs << colorToHex(roleColors[i]) << "\n";
            }
            ofs.close();
        } catch(...) {}
    }
    // fonts config (load/save)
    void loadFontsConfig() {
        try {
            std::ifstream ifs("fonts.cfg");
            if (!ifs) return;
            std::string line;
            if (std::getline(ifs, line) && !line.empty()) fontPathEN = line;
            if (std::getline(ifs, line) && !line.empty()) fontPathCN = line;
        } catch(...) {}
    }
    void saveFontsConfig() {
        try {
            std::ofstream ofs("fonts.cfg");
            if (!ofs) return;
            ofs << fontPathEN << "\n" << fontPathCN << "\n";
        } catch(...) {}
    }

    // Character script detection (basic CJK ranges)
    static bool isWide(sf::Uint32 ch) {
        if((ch >= 0x4E00 && ch <= 0x9FFF) || // CJK Unified Ideographs
           (ch >= 0x3400 && ch <= 0x4DBF) || // CJK Unified Ideographs Extension A
           (ch >= 0x20000 && ch <= 0x2A6DF) || // CJK Unified Ideographs Extension B
           (ch >= 0x2A700 && ch <= 0x2B73F) || // CJK Unified Ideographs Extension C
           (ch >= 0x2B740 && ch <= 0x2B81F) || // CJK Unified Ideographs Extension D
           (ch >= 0x2B820 && ch <= 0x2CEAF) || // CJK Unified Ideographs Extension E
           (ch >= 0xF900 && ch <= 0xFAFF) || // CJK Compatibility Ideographs
           (ch >= 0x2F800 && ch <= 0x2FA1F) || // CJK Compatibility Ideographs Supplement
           (ch >= 0x3040 && ch <= 0x309F) || // Hiragana
           (ch >= 0x30A0 && ch <= 0x30FF) || // Katakana
           (ch >= 0xAC00 && ch <= 0xD7AF) || // Hangul Syllables
           (ch >= 0x3000 && ch <= 0x303F) ||   // CJK Symbols and Punctuation
           (ch >= 0xFF00 && ch <= 0xFFEF))     // Halfwidth and Fullwidth Forms
            return true;
        return false;
    }
    // return font pointer for a char
    sf::Font* getFontForChar(sf::Uint32 ch) const {
        return isWide(ch) ? const_cast<sf::Font*>(&fontCN) : const_cast<sf::Font*>(&font);
    }
    float getGlyphAdvance(sf::Uint32 ch, unsigned int charSize) const {
        sf::Font* f = getFontForChar(ch);
        return f->getGlyph(ch, charSize, false).advance;
    }

#ifdef _WIN32
    // open file picker, returns utf8 path or empty
    std::string openFilePicker(const wchar_t* filter = L"All Files\0*.*\0\0") {
        OPENFILENAMEW ofn;
        wchar_t szFile[MAX_PATH] = {0};
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            // convert wide to utf8
            std::wstring ws(szFile);
            std::string out;
            for (wchar_t wc : ws) {
                if (wc <= 0x7F) out.push_back((char)wc);
                else {
                    // use simple conversion via wchar->utf8
                    uint32_t ch = (uint32_t)wc;
                    if (ch <= 0x7FF) {
                        out.push_back((char)(0xC0 | ((ch >> 6) & 0x1F)));
                        out.push_back((char)(0x80 | (ch & 0x3F)));
                    } else {
                        out.push_back((char)(0xE0 | ((ch >> 12) & 0x0F)));
                        out.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (ch & 0x3F)));
                    }
                }
            }
            return out;
        }
        return std::string();
    }
#else
    std::string openFilePicker(const wchar_t* = nullptr) { return std::string(); }
#endif
    void updateSlidersFromRole(int ri) {
        sf::Color c = roleColors[ri];
        sliderValueInt[0] = c.r; sliderValueInt[1] = c.g; sliderValueInt[2] = c.b;
        hexInputText = colorToHex(c);
    }
    void updateRoleFromSliders(int ri) {
        roleColors[ri] = sf::Color((uint8_t)sliderValueInt[0], (uint8_t)sliderValueInt[1], (uint8_t)sliderValueInt[2]);
    }
    sf::RectangleShape box;
    sf::Font font; // 英文字体（默认）
    sf::Font fontCN; // 中文字体
    sf::Text text, lineNumText;
    sf::RectangleShape cursorShape;

    std::vector<sf::String> lines;
    Cursor cursor;
    Selection selection;

    float padding = 8.f;
    float lineHeight;
    float lineNumberWidth = 48.f;

    // 水平滚动（像素）
    float hScroll = 0.f;
    // 水平滚动额外缓冲（像素），在 resize/updateLayout 时动态调整
    float hScrollMargin = 40.f;

    // compile output toggle button size
    float compileToggleButtonW = 64.f;
    float compileToggleButtonH = 20.f;

    std::size_t scrollOffset = 0;
    std::size_t visibleLines = 1;
    // tab bar height reserved at top of the box (world units)
    float tabBarHeight = 28.f;
    // preserved desired visual column for Up/Down navigation (-1 = unset)
    int desiredVisualCol = -1;
    // tab display width in visual cells (user-configurable)
    int tabDisplayWidth = 4;

    sf::Clock blinkClock;
    bool cursorVisible = true;
    bool focused = false;
    bool mouseDown = false;
    std::string currentFile;
    int FileIdCounter = 0;
    struct OpenFile {
        std::string path;
        std::vector<sf::String> lines;
        int id;
        bool modified;
        std::stack<EditAction> undoStack, redoStack;
        Cursor cursor;
        Selection selection;
        // 起始视图行（切换文件时恢复）
        std::size_t viewStart = 0;
    };
    std::vector<OpenFile> openFiles;
    int activeFileIndex = -1;
    std::vector<sf::FloatRect> tabRects;
    std::vector<int> tabIndexMap; // maps tabRects index -> openFiles index
    sf::FloatRect rightTabRect; // rect for the right-pane's tab drawn near top-right
    // tab dragging state: index being dragged, offset from mouse to tab origin, and whether we've started dragging
    int draggingTabIndex = -1;
    sf::Vector2f draggingTabOffset = {0.f,0.f};
    bool isTabDragging = false;
    sf::Vector2f draggedTabPos = {0.f,0.f};
    sf::FloatRect editDefaultBtnRect, editCompileBtnRect;
    // Settings UI
    bool settingsVisible = false;
    sf::FloatRect settingsBtnRect;
    sf::FloatRect settingsWindowRect;
    // custom font buttons
    sf::FloatRect settings_CustomFontENRect;
    sf::FloatRect settings_CustomFontCNRect;
    // controls inside settings popup
    sf::FloatRect settings_EditDefaultRect;
    sf::FloatRect settings_EditCompileRect;
    sf::FloatRect settings_ToggleOutputRect;
    sf::FloatRect settings_TabDecRect;
    sf::FloatRect settings_TabIncRect;
    // host window pointer for updating title and drawing overlays
    sf::RenderWindow* hostWindow = nullptr;
    // 分屏相关
    bool splitView = false; // Ctrl+M 切换
    bool splitDiffEnabled = true; // 设置中是否在分屏时启用 diff
    int splitOtherIndex = -1; // 分屏时右侧显示的文件索引
    int splitLeftIndex = -1; // 分屏时左侧显示的文件索引
    // 分屏时哪个窗格拥有编辑焦点：-1 none, 0 left, 1 right
    int splitPaneFocused = -1;
    std::vector<bool> diffLeft, diffRight; // 差异标记
    sf::FloatRect settings_SplitDiffRect;

    // Color customization
    enum ColorRole {
        CR_Background = 0,
        CR_Text,
        CR_Button,
        CR_CompileBg,
        CR_CompileFg,
        CR_Keyword,
        CR_KeywordExt,
        CR_Symbol,
        CR_Preprocessor,
        CR_Comment,
        CR_String,
        CR_Hex,
        CR_Oct,
        CR_Float,
        CR_LineNum,
        CR_ButtonText,
        CR_SettingsText,
        CR_SettingsBg,
        CR_Int,
        CR_FileTabsBackground_Active,
        CR_FileTabsBackground_Inactive,
        CR_AfterDefinition,
        CR_COUNT
    };
    
    const char* roleNames[CR_COUNT] = {
        "Background","Text","Button","CompileBg","CompileFg",
        "Keyword","KeywordExt","Symbol","Preprocessor","Comment",
        "String","Hex","Oct","Float",
        "LineNum","ButtonText","SettingsText","SettingsBg","Int","FileTabsBgA","FileTabsBgI","AfterDefinition"
    };
    sf::Color roleColors[CR_COUNT];
    std::vector<sf::FloatRect> colorSwatchRects;
    // simple preset palette users can cycle through by clicking a swatch
    std::vector<sf::Color> colorPresets;
    // settings: whether color editor panel is visible inside the settings popup
    bool colorEditorVisible = true;
    sf::FloatRect settings_ColorTabRect;
    // editor for selected color: sliders and hex input
    int selectedColorRole = 0;
    int sliderValueInt[3] = {0,0,0};
    sf::FloatRect sliderRects[3];
    sf::FloatRect sliderTrackRects[3];
    sf::FloatRect hexInputRect;
    std::string hexInputText;
    bool hexFocused = false;
    int activeSlider = -1; // 0=R,1=G,2=B
    bool draggingSlider = false;

    // font paths (saved/loaded)
    std::string fontPathEN = "sarasa-mono.ttf";
    std::string fontPathCN = "sarasa-mono.ttf";

    // custom compiler settings
    std::string gppPath = "g++";
    std::string extraFlags = "-std=c++17 -O2 -Wall -Wextra";
    std::string compileTemplate; // if set, can contain %FILE% and %OUT%

    // compile output storage
    std::vector<std::string> compileOutputLines;
    bool compileOutputVisible = false;
    // which line of compile output is shown at top
    std::size_t compileOutputFirstLine = 0;

public:
    TextBox(sf::Vector2f pos, sf::Vector2f size, sf::RenderWindow* host = nullptr)
        : text(font, "", 20),
          lineNumText(font, "", 20)
    {
        hostWindow = host;
        // load saved font paths and try to open fonts; fall back to bundled font name
        loadFontsConfig();
        bool enLoaded = false, cnLoaded = false;
        try {
            enLoaded = font.openFromFile(fontPathEN);
        } catch(...) { enLoaded = false; }
        try {
            cnLoaded = fontCN.openFromFile(fontPathCN);
        } catch(...) { cnLoaded = false; }
        // fallback: if failure, try default sarasa-mono.ttf for EN and CN
        if (!enLoaded) {
            try { enLoaded = font.openFromFile("sarasa-mono.ttf"); fontPathEN = "sarasa-mono.ttf"; } catch(...) { enLoaded = false; }
        }
        if (!cnLoaded) {
            try { cnLoaded = fontCN.openFromFile("sarasa-mono.ttf"); fontPathCN = "sarasa-mono.ttf"; } catch(...) { cnLoaded = false; }
        }
        // persist any defaulted paths
        saveFontsConfig();

        // prepare preset palette
        colorPresets = {
            sf::Color(0,0,0), sf::Color(255,255,255), sf::Color(30,30,30), sf::Color(250,250,250),
            sf::Color(220,220,240), sf::Color(200,200,200), sf::Color(120,120,120),
            sf::Color(28,82,199), sf::Color(43,145,175), sf::Color(110,169,61),
            sf::Color(153,0,85), sf::Color(160,160,160), sf::Color(206,123,0),
            sf::Color(64,182,159), sf::Color(203,58,106), sf::Color(128,0,128),
            sf::Color(220,220,240), sf::Color(230,230,230)
        };

        // default role colors (matching previous hardcoded defaults)
        for (int i = 0; i < CR_COUNT; ++i) roleColors[i] = sf::Color::Black;
        roleColors[CR_Background] = sf::Color(250,250,250);
        roleColors[CR_Text] = sf::Color(0,0,0); // placeholder, will set below
        roleColors[CR_Button] = sf::Color(200,200,220);
        roleColors[CR_CompileBg] = sf::Color(30,30,30,220);
        roleColors[CR_CompileFg] = sf::Color::White;
        roleColors[CR_Keyword] = sf::Color(28,82,199);
        roleColors[CR_KeywordExt] = sf::Color(43,145,175);
        roleColors[CR_Symbol] = sf::Color(110,169,61);
        roleColors[CR_Preprocessor] = sf::Color(153,0,85);
        roleColors[CR_Comment] = sf::Color(160,160,160);
        roleColors[CR_String] = sf::Color(206,123,0);
        roleColors[CR_Hex] = sf::Color(64,182,159);
        roleColors[CR_Oct] = sf::Color(203,58,106);
        roleColors[CR_Float] = sf::Color(128,0,128);
        // new roles defaults
        roleColors[CR_LineNum] = sf::Color(120,120,120);
        roleColors[CR_ButtonText] = sf::Color::Black;
        roleColors[CR_SettingsText] = sf::Color::Black;
        roleColors[CR_SettingsBg] = sf::Color(245,245,245);
        roleColors[CR_Int] = sf::Color(128,0,128);
        roleColors[CR_FileTabsBackground_Active] = sf::Color(220,220,240);
        roleColors[CR_FileTabsBackground_Inactive] = sf::Color(230,230,230);
        roleColors[CR_AfterDefinition] = sf::Color(30,30,190);
        // ensure default Text follows SettingsText (non-highlighted code text)
        roleColors[CR_Text] = roleColors[CR_SettingsText];
        // try load saved colors from disk (overrides defaults)
        loadColors();
        // initialize editor sliders/hex for selected role
        updateSlidersFromRole(selectedColorRole);

        box.setPosition(pos);
        box.setSize(size);
        box.setFillColor(roleColors[CR_Background]);
        box.setOutlineThickness(2.f);
        box.setOutlineColor(sf::Color::Black);

        text.setFillColor(roleColors[CR_Text]);
        lineNumText.setFillColor(roleColors[CR_LineNum]);

        lineHeight = text.getCharacterSize() + 6.f;

        cursorShape.setSize({2.f, (float)text.getCharacterSize()});
        cursorShape.setFillColor(roleColors[CR_Text]);

        lines.emplace_back("");
        updateLayout();

        // initialize hScrollMargin based on initial size
        hScrollMargin = std::max(16.f, box.getSize().x * 0.05f);

        // try to load compile template from compile.txt if exists
        try {
            std::ifstream ifs("compile.txt");
            if (ifs) {
                std::string line;
                if (std::getline(ifs, line)) {
                    if (!line.empty()) compileTemplate = line;
                }
            }
        } catch(...) {}
        // try to load default source path from default_source.cpp and load that file
        try {
            std::ifstream dfs("default_source.cpp");
            if (dfs) {
                std::filesystem::path fp("default_source.cpp");
                if (std::filesystem::exists(fp)) {
                    std::ifstream src(fp);
                    if (src) {
                        lines.clear();
                        std::string l;
                        while (std::getline(src, l)) {
                            lines.emplace_back(utf8_decode_string(l));
                        }
                        if (lines.empty()) lines.emplace_back("");
                            // set currentFile remains empty if loading default snippet
                    }
                }
            }
        } catch(...) {}
            // register initial open file (either loaded default source or empty)
            {
                OpenFile f;
                f.path = currentFile;
                f.lines = lines;
                f.id = ++FileIdCounter;
                // if currentFile is empty then content likely from default snippet -> mark modified
                f.modified = currentFile.empty();
                f.cursor = cursor;
                f.viewStart = scrollOffset;
                f.selection = selection;
                openFiles.push_back(std::move(f));
                activeFileIndex = 0;
            }
        // initial window title update
        if (hostWindow) updateWindowTitle();
    }

        // sync current state back to the active OpenFile entry
        void syncCurrentToOpenFiles() {
            if (activeFileIndex >= 0 && activeFileIndex < (int)openFiles.size()) {
                openFiles[activeFileIndex].lines = lines;
                openFiles[activeFileIndex].cursor = cursor;
                openFiles[activeFileIndex].viewStart = scrollOffset;
                openFiles[activeFileIndex].selection = selection;
                if (splitView && splitDiffEnabled) {
                    // 如果分屏并启用 diff，更新差异信息（使用 splitLeftIndex 作为左侧）
                    int left = (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex);
                    int other = splitOtherIndex;
                    if (other < 0 && openFiles.size() >= 2) other = (left + 1) % openFiles.size();
                    if (left >= 0 && left < (int)openFiles.size() && other >= 0 && other < (int)openFiles.size())
                        computeDiff(openFiles[left].lines, openFiles[other].lines, diffLeft, diffRight);
                }
            }
        }

        void markModified(bool v = true) {
            if (activeFileIndex >= 0 && activeFileIndex < (int)openFiles.size())
                openFiles[activeFileIndex].modified = v;
        }

        bool promptSaveIfModified(int idx) {
            if (idx < 0 || idx >= (int)openFiles.size()) return true;
            if (!openFiles[idx].modified) return true;
    #ifdef _WIN32
            std::string name = openFiles[idx].path.empty() ? "Untitled "+std::__cxx11::to_string(openFiles[idx].id) : std::filesystem::path(openFiles[idx].path).filename().string();
            std::string msg = "Save changes to '" + name + "'?";
            int res = MessageBoxA(NULL, msg.c_str(), "Save", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (res == IDYES) {
                switchToOpenFile(idx);
                // attempt save
                if (currentFile.empty()) saveAsDialog(); else save();
                return !openFiles[activeFileIndex].modified;
            } else if (res == IDNO) {
                return true; // discard changes
            } else {
                return false; // cancel
            }
    #else
            // non-windows fallback: behave as if "Yes"
            switchToOpenFile(idx);
            if (currentFile.empty()) saveAsDialog(); else save();
            return !openFiles[activeFileIndex].modified;
    #endif
        }
        int promptSaveIfModified(int idx,bool GETRESULT)
        {
            if(!GETRESULT)
            {
                return promptSaveIfModified(idx)?1:0;
            }
            if (idx < 0 || idx >= (int)openFiles.size()) return true;
            if (!openFiles[idx].modified) return true;
    #ifdef _WIN32
            std::string name = openFiles[idx].path.empty() ? "Untitled "+std::__cxx11::to_string(openFiles[idx].id) : std::filesystem::path(openFiles[idx].path).filename().string();
            std::string msg = "Save changes to '" + name + "'?";
            int res = MessageBoxA(NULL, msg.c_str(), "Save", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (res == IDYES) {
                switchToOpenFile(idx);
                // attempt save
                if (currentFile.empty()) saveAsDialog(); else save();
                return !openFiles[activeFileIndex].modified;
            } else if (res == IDNO) {
                return 2; // discard changes
            } else {
                return 3; // cancel
            }
    #else
            // non-windows fallback: behave as if "Yes"
            switchToOpenFile(idx);
            if (currentFile.empty()) saveAsDialog(); else save();
            return !openFiles[activeFileIndex].modified;
    #endif
        }

        // switch view to an open file by index
        void switchToOpenFile(int idx) {
            if (idx < 0 || idx >= (int)openFiles.size()) return;
            // save current into list
            if (activeFileIndex >= 0 && activeFileIndex < (int)openFiles.size()) {
                openFiles[activeFileIndex].lines = lines;
                openFiles[activeFileIndex].cursor = cursor;
                openFiles[activeFileIndex].viewStart = scrollOffset;
                openFiles[activeFileIndex].selection = selection;
            }
            activeFileIndex = idx;
            lines = openFiles[idx].lines;
            currentFile = openFiles[idx].path;
            cursor = openFiles[idx].cursor;
            selection = openFiles[idx].selection;
            // 恢复该文件的视图起始行（而不是立即强制使光标可见）
            scrollOffset = openFiles[idx].viewStart;
            resetCursorBlink();
            clampScroll();
            clampCursor();
            if (splitView && splitDiffEnabled) {
                if (openFiles.size() >= 1) {
                    if (splitLeftIndex < 0) splitLeftIndex = activeFileIndex;
                    if (splitOtherIndex < 0 || splitOtherIndex >= (int)openFiles.size())
                        splitOtherIndex = (splitLeftIndex + 1) % openFiles.size();
                    computeDiff(openFiles[splitLeftIndex].lines, openFiles[splitOtherIndex].lines, diffLeft, diffRight);
                }
            }
            updateWindowTitle();
        }

        // focus a file for editing without altering splitLeft/Other indices
        void focusFile(int idx) {
            if (idx < 0 || idx >= (int)openFiles.size()) return;
            if (activeFileIndex >= 0 && activeFileIndex < (int)openFiles.size()) {
                openFiles[activeFileIndex].lines = lines;
                openFiles[activeFileIndex].cursor = cursor;
                openFiles[activeFileIndex].viewStart = scrollOffset;
                openFiles[activeFileIndex].selection = selection;
            }
            activeFileIndex = idx;
            lines = openFiles[idx].lines;
            currentFile = openFiles[idx].path;
            cursor = openFiles[idx].cursor;
            selection = openFiles[idx].selection;
            scrollOffset = openFiles[idx].viewStart;
            resetCursorBlink();
            clampScroll();
            clampCursor();
            updateWindowTitle();
        }

        // load default source file referenced by default_source.txt; returns lines (utf8-decoded)
        std::vector<sf::String> loadDefaultSourceLines() {
            std::vector<sf::String> out;
            try {
                wchar_t strSelf[MAX_PATH];
                memset(strSelf, 0, sizeof(strSelf));
                GetModuleFileNameW(NULL, strSelf, sizeof(strSelf));
                std::wstring WcurrentDir = strSelf;
                for(auto it=WcurrentDir.rbegin(); it!=WcurrentDir.rend(); ++it) {
                    if (*it == L'\\' || *it == L'/') {
                        WcurrentDir.erase(it.base()-1, WcurrentDir.end());
                        break;
                    }
                }
                WcurrentDir += L"\\";
                std::string currentDir = utf8_encode_string(WcurrentDir);
                std::ifstream dfs((currentDir + "default_source.cpp").c_str());
                if (dfs) {
                    std::filesystem::path fp((currentDir + "default_source.cpp").c_str());
                    if (std::filesystem::exists(fp)) {
                        std::ifstream src(fp);
                        std::string l;
                        while (std::getline(src, l)) out.emplace_back(utf8_decode_string(l));
                        if (out.empty()) out.emplace_back("");
                        return out;
                    }
                }
            } catch(...) {}
            out.emplace_back("");
            return out;
        }

        void createNewFile() {
            syncCurrentToOpenFiles();
            OpenFile f;
            f.path = std::string();
            f.lines = loadDefaultSourceLines();
            f.id = ++FileIdCounter;
            f.modified = true;
            f.cursor = {0,0};
            f.selection = Selection();
            f.viewStart = 0;
            openFiles.push_back(std::move(f));
            switchToOpenFile((int)openFiles.size()-1);
        }

        void closeCurrentFile() {
            if (activeFileIndex < 0 || activeFileIndex >= (int)openFiles.size()) return;
            // sync current edits back
            syncCurrentToOpenFiles();
            if (!promptSaveIfModified(activeFileIndex)) return;
            // remove the active file
            int removedIdx = activeFileIndex;
            openFiles.erase(openFiles.begin() + removedIdx);
            // if split view was enabled, adjust or disable if one of split files was removed
            if (splitView) {
                if (removedIdx == splitLeftIndex || removedIdx == splitOtherIndex) {
                    // one of the split files was closed -> disable split
                    splitView = false;
                    splitLeftIndex = -1;
                    splitOtherIndex = -1;
                } else {
                    // adjust indices that were after removed index
                    if (splitLeftIndex > removedIdx) splitLeftIndex--;
                    if (splitOtherIndex > removedIdx) splitOtherIndex--;
                    if (openFiles.size() < 2) {
                        splitView = false;
                        splitLeftIndex = -1;
                        splitOtherIndex = -1;
                    }
                }
            }
            // choose new active index
            if (openFiles.empty()) {
                OpenFile f;
                f.path = std::string();
                f.lines=loadDefaultSourceLines();
                f.id = ++FileIdCounter;
                f.cursor = {0,0};
                f.selection = Selection();
                f.viewStart = 0;
                openFiles.push_back(std::move(f));
                activeFileIndex = 0;
            } else {
                if (activeFileIndex >= (int)openFiles.size()) activeFileIndex = (int)openFiles.size() - 1;
            }
            // switch view to new active
            lines = openFiles[activeFileIndex].lines;
            currentFile = openFiles[activeFileIndex].path;
            cursor = openFiles[activeFileIndex].cursor;
            selection = openFiles[activeFileIndex].selection;
            // 恢复该文件的视图起始行
            scrollOffset = openFiles[activeFileIndex].viewStart;
            clampScroll();
            clampCursor();
            updateWindowTitle();
        }

    void openDefaultSource() {
        // determine executable directory and default_source.txt path
#ifdef _WIN32
        wchar_t buf[MAX_PATH]; memset(buf,0,sizeof(buf));
        GetModuleFileNameW(NULL, buf, sizeof(buf));
        std::wstring WcurrentDir = buf;
        for (auto it = WcurrentDir.rbegin(); it != WcurrentDir.rend(); ++it) {
            if (*it == L'\\' || *it == L'/') {
                WcurrentDir.erase(it.base()-1, WcurrentDir.end());
                break;
            }
        }
        WcurrentDir += L"\\";
        std::string currentDir = utf8_encode_string(WcurrentDir);
        std::string path = currentDir + "default_source.cpp";
        // open it in editor (adds to openFiles)
        openFromFilePath(path);
#else
        // fallback: try local file
        std::string path = std::string("default_source.cpp");
        openFromFilePath(path);
#endif
    }

    void openCompileTXT() {
#ifdef _WIN32
        wchar_t buf[MAX_PATH]; memset(buf,0,sizeof(buf));
        GetModuleFileNameW(NULL, buf, sizeof(buf));
        std::wstring WcurrentDir = buf;
        for (auto it = WcurrentDir.rbegin(); it != WcurrentDir.rend(); ++it) {
            if (*it == L'\\' || *it == L'/') {
                WcurrentDir.erase(it.base()-1, WcurrentDir.end());
                break;
            }
        }
        WcurrentDir += L"\\";
        std::string currentDir = utf8_encode_string(WcurrentDir);
        std::string path = currentDir + "compile.txt";
        // open it in editor (adds to openFiles)
        openFromFilePath(path);
#else
        // fallback: try local file
        std::string path = std::string("compile.txt");
        openFromFilePath(path);
#endif
    }

    /* =================== 窗口自适应 =================== */

    void setArea(sf::Vector2f pos, sf::Vector2f size) {
        box.setPosition(pos);
        box.setSize(size);
        updateLayout();
    }

    void updateLayout() {
        // reserve space for padding and the tab bar when computing visible lines
        float contentH = box.getSize().y - padding*2.f - tabBarHeight;
        // if compile output overlay is visible and overlaps the editor box, subtract its overlap
        if (compileOutputVisible && hostWindow) {
            sf::Vector2u winSz = hostWindow->getSize();
            float pad = 10.f;
            float overlayHpx = std::min(200.f, (float)winSz.y * 0.25f);
            float tlPixelY = (float)winSz.y - overlayHpx - pad;
            sf::Vector2f tlWorld = hostWindow->mapPixelToCoords({(int)std::lround(pad), (int)std::lround(tlPixelY)});
            float overlayTopY = tlWorld.y;
            float boxTopY = box.getPosition().y;
            float boxBottomY = boxTopY + box.getSize().y;
            if (overlayTopY < boxBottomY) {
                float overlap = boxBottomY - std::max(overlayTopY, boxTopY);
                if (overlap > 0.f) contentH -= overlap;
            }
        }
        visibleLines = std::max<std::size_t>(1, (std::size_t)std::max(1.f, std::floor(contentH / lineHeight)));
        // update horizontal scroll margin relative to window pixel width (converted to world units)
        if (hostWindow) {
            sf::Vector2u winSz = hostWindow->getSize();
            float pixelMargin = std::max(16.f, (float)winSz.x * 0.05f);
            sf::Vector2f w0 = hostWindow->mapPixelToCoords({0,0});
            sf::Vector2f w1 = hostWindow->mapPixelToCoords({(int)std::lround(pixelMargin), 0});
            hScrollMargin = std::abs(w1.x - w0.x);
        } else {
            hScrollMargin = std::max(16.f, box.getSize().x * 0.05f);
        }
        clampScroll();
        clampCursor();
    }

    void updateWindowTitle() {
        if (!hostWindow) return;
        std::string title = LEMONCPP_AND_VERSION;
        if (!currentFile.empty()) title += " - " + currentFile;
        hostWindow->setTitle(title);
        compileOutputFirstLine = 0;
    }

    /* =================== 事件处理 =================== */

    void handleEvent(const sf::Event& event, sf::RenderWindow& window) {
        if (event.getIf<sf::Event::Closed>()) {
            // prompt for each modified file; if any cancel, abort closing
            for (int i = 0; i < (int)openFiles.size(); ++i) {
                if (openFiles[i].modified) {
                    if (!promptSaveIfModified(i)) return;
                }
            }
            if (hostWindow) hostWindow->close();
            return;
        }
        if (event.getIf<sf::Event::Resized>()) {
            // map pixel bounds to world coordinates so the editor area stays aligned
            sf::Vector2u winSz = window.getSize();
            sf::View uiView;
            uiView.setSize({(float)winSz.x, (float)winSz.y});
            uiView.setCenter({(float)winSz.x * 0.5f, (float)winSz.y * 0.5f});
            window.setView(uiView);
            sf::Vector2i tlPixel(10, 10);
            sf::Vector2i brPixel((int)winSz.x - 10, (int)winSz.y - 10);
            sf::Vector2f tlWorld = window.mapPixelToCoords(tlPixel);
            sf::Vector2f brWorld = window.mapPixelToCoords(brPixel);
            setArea(tlWorld, brWorld - tlWorld);
        }

        if (event.getIf<sf::Event::MouseButtonPressed>()) {
            auto mpixel = sf::Mouse::getPosition(window);
            sf::Vector2f m = window.mapPixelToCoords(mpixel);
            // check if click is on a tab (start potential drag; actual switch on release if not dragged)
            for (std::size_t ti = 0; ti < tabRects.size(); ++ti) {
                if (tabRects[ti].contains(m)) {
                    // map tab rect index to the actual openFiles index
                    if (ti < tabIndexMap.size()) {
                        draggingTabIndex = tabIndexMap[ti];
                        draggingTabOffset = sf::Vector2f(m.x - tabRects[ti].position.x, m.y - tabRects[ti].position.y);
                        isTabDragging = false;
                        // consume the click (we'll handle switch on release)
                        return;
                    }
                }
            }
            // if clicked on right-side tab, start dragging or select
            if (rightTabRect.contains(m) && splitView && splitOtherIndex >= 0 && splitOtherIndex < (int)openFiles.size()) {
                draggingTabIndex = splitOtherIndex;
                draggingTabOffset = sf::Vector2f(m.x - rightTabRect.position.x, m.y - rightTabRect.position.y);
                isTabDragging = false;
                return;
            }
            // Settings button / popup handling
            if (settingsBtnRect.contains(m)) {
                settingsVisible = !settingsVisible;
                return; // don't focus editor when toggling settings
            }
            if (settingsVisible) {
                // if click inside popup, handle sub-controls
                if (settingsWindowRect.contains(m)) {
                    // check color swatch clicks (cycle preset on click)
                    for (int ri = 0; ri < (int)colorSwatchRects.size(); ++ri) {
                        if (colorSwatchRects[ri].contains(m)) {
                            // select this role for editing
                            selectedColorRole = ri;
                            updateSlidersFromRole(ri);
                            hexFocused = false;
                            return;
                        }
                    }
                    if (settings_EditDefaultRect.contains(m)) {
                        openDefaultSource();
                        settingsVisible = false;
                        return;
                    }
                    if (settings_ColorTabRect.contains(m)) {
                        colorEditorVisible = !colorEditorVisible;
                        return;
                    }

                    if (settings_EditCompileRect.contains(m)) {
                        openCompileTXT();
                        settingsVisible = false;
                        return;
                    }
                    if (settings_SplitDiffRect.contains(m)) {
                        splitDiffEnabled = !splitDiffEnabled;
                        if (splitView && splitDiffEnabled) {
                            if (openFiles.size() >= 1) {
                                if (splitLeftIndex < 0) splitLeftIndex = activeFileIndex;
                                if (splitOtherIndex < 0 || splitOtherIndex >= (int)openFiles.size()) splitOtherIndex = (splitLeftIndex + 1) % openFiles.size();
                                computeDiff(openFiles[splitLeftIndex].lines, openFiles[splitOtherIndex].lines, diffLeft, diffRight);
                            }
                        }
                        updateLayout();
                        return;
                    }
                    // custom font pick buttons hit-testing (rects are set during draw)
                    if (settings_CustomFontENRect.contains(m)) {
                        std::string path = openFilePicker();
                        if (!path.empty()) {
                            if (font.openFromFile(path)) {
                                fontPathEN = path;
                                saveFontsConfig();
                            }
                        }
                        settingsVisible = false;
                        return;
                    }
                    if (settings_CustomFontCNRect.contains(m)) {
                        std::string path = openFilePicker();
                        if (!path.empty()) {
                            if (fontCN.openFromFile(path)) {
                                fontPathCN = path;
                                saveFontsConfig();
                            }
                        }
                        settingsVisible = false;
                        return;
                    }
                    // check slider knobs and hex input click
                    for (int si=0; si<3; ++si) {
                        if (sliderRects[si].contains(m)) {
                            activeSlider = si;
                            draggingSlider = true;
                            // initialize sliderValueInt already set when selecting role
                            return;
                        }
                    }
                    if (hexInputRect.contains(m)) {
                        hexFocused = true;
                        return;
                    } else {
                        hexFocused = false;
                    }
                    if (settings_ToggleOutputRect.contains(m)) {
                        compileOutputVisible = !compileOutputVisible;
                        if (compileOutputVisible) compileOutputFirstLine = 0;
                        updateLayout();
                        settingsVisible = false;
                        return;
                    }
                    if (settings_TabDecRect.contains(m)) {
                        setTabWidth(tabDisplayWidth - 1);
                        return;
                    }
                    if (settings_TabIncRect.contains(m)) {
                        setTabWidth(tabDisplayWidth + 1);
                        return;
                    }
                    // click inside popup but not on a control -> consume
                    return;
                } else {
                    // click outside popup closes it (unless clicking settings button which handled above)
                    settingsVisible = false;
                }
            }
            // check if click is on compile output toggle button (unchanged location)
            sf::FloatRect btnRect(
                {box.getPosition().x + box.getSize().x - padding - compileToggleButtonW,
                box.getPosition().y + padding*0.5f},
                {compileToggleButtonW, compileToggleButtonH}
            );
            if (btnRect.contains(m)) {
                compileOutputVisible = !compileOutputVisible;
                if (compileOutputVisible) compileOutputFirstLine = 0;
                updateLayout();
                return; // don't focus editor when clicking the button
            }
            // 分屏点击：如果在分屏模式下，检测点击发生在左/右面板并把焦点和光标设置到对应文件
            if (splitView) {
                float paneLeft = box.getPosition().x + padding;
                float paneTop = box.getPosition().y + padding + tabBarHeight;
                float paneW = box.getSize().x - padding*2.f;
                float contentH = box.getSize().y - padding*2.f - tabBarHeight;
                float gap = 6.f;
                float leftW = std::max(10.f, paneW * 0.5f - gap);
                float rightW = std::max(10.f, paneW - leftW - gap);
                // map mouse pixel to world coords for consistent calculations
                sf::Vector2i mp = sf::Mouse::getPosition(window);
                sf::Vector2f mw = window.mapPixelToCoords(mp);
                // left panel world rect
                float leftWorldX = paneLeft;
                float topWorldY = paneTop;
                float leftWorldW = leftW;
                float leftWorldH = contentH;
                // right panel world rect
                float rightWorldX = paneLeft + leftW + gap;
                float rightWorldW = rightW;
                // left panel
                if (mw.x >= leftWorldX && mw.x < leftWorldX + leftWorldW && mw.y >= topWorldY && mw.y < topWorldY + leftWorldH) {
                    int leftIndex = (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex);
                    if (leftIndex < 0 || leftIndex >= (int)openFiles.size()) {
                        // nothing to do
                    } else {
                        // switch to left file and compute cursor position within that pane
                        syncCurrentToOpenFiles();
                        focusFile(leftIndex);
                        // compute local world coords relative to content area
                        float localX = mw.x - (paneLeft + lineNumberWidth);
                        float localY = mw.y - paneTop;
                        splitPaneFocused = 0;
                        std::size_t linesPerPanel = std::max<std::size_t>(1, (std::size_t)std::max(1.f, std::floor(contentH / lineHeight)));
                        std::size_t leftScroll = openFiles[leftIndex].viewStart;
                        auto &L = openFiles[leftIndex].lines;
                        std::size_t maxStart = 0; if (L.size() > linesPerPanel) maxStart = L.size() - linesPerPanel;
                        if (leftScroll > maxStart) leftScroll = maxStart;
                        std::size_t clickRow = leftScroll + (std::size_t)std::floor(localY / lineHeight);
                        if (clickRow >= L.size()) clickRow = L.size() - 1;
                        // compute column by measuring glyph widths
                        float relX = localX + hScroll;
                        if (relX < 0.f) relX = 0.f;
                        sf::String &lineStr = L[clickRow];
                        std::size_t n = lineStr.getSize();
                        float spaceAdvance = getGlyphAdvance(L' ', text.getCharacterSize());
                        float acc = 0.f;
                        std::size_t col = 0;
                        for (std::size_t i = 0;i < n; ++i) {
                            sf::Uint32 ch = lineStr[i];
                            float w = (ch==L'\t') ? spaceAdvance * tabDisplayWidth : getGlyphAdvance(ch, text.getCharacterSize());
                            if (acc + w*0.5f >= relX) { col = i; break; }
                            acc += w;
                            col = i+1;
                        }
                        cursor.row = clickRow;
                        cursor.col = col;
                        scrollOffset = leftScroll;
                        // 写回到该文件的持久状态，确保分屏各自显示独立光标/选区
                        if (leftIndex >= 0 && leftIndex < (int)openFiles.size()) {
                            openFiles[leftIndex].cursor = cursor;
                            openFiles[leftIndex].selection = selection;
                            openFiles[leftIndex].viewStart = leftScroll;
                        }
                        resetCursorBlink();
                        focused = true;
                        mouseDown = true;
                        selection.active = true;
                        selection.start = cursor;
                        selection.end = cursor;
                        return;
                    }
                }
                // right panel
                if (mw.x >= rightWorldX && mw.x < rightWorldX + rightWorldW && mw.y >= topWorldY && mw.y < topWorldY + leftWorldH) {
                    int rightIndex = (splitOtherIndex >= 0 ? splitOtherIndex : (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex));
                    if (rightIndex < 0 || rightIndex >= (int)openFiles.size()) {
                        // nothing
                    } else {
                        syncCurrentToOpenFiles();
                        focusFile(rightIndex);
                        float localX = mw.x - (paneLeft + leftW + gap + lineNumberWidth);
                        float localY = mw.y - paneTop;
                        splitPaneFocused = 1;
                        std::size_t linesPerPanel = std::max<std::size_t>(1, (std::size_t)std::max(1.f, std::floor(contentH / lineHeight)));
                        std::size_t rightScroll = openFiles[rightIndex].viewStart;
                        auto &R = openFiles[rightIndex].lines;
                        std::size_t maxStartR = 0; if (R.size() > linesPerPanel) maxStartR = R.size() - linesPerPanel;
                        if (rightScroll > maxStartR) rightScroll = maxStartR;
                        std::size_t clickRow = rightScroll + (std::size_t)std::floor(localY / lineHeight);
                        if (clickRow >= R.size()) clickRow = R.size() - 1;
                        float relX = localX - lineNumberWidth + hScroll;
                        if (relX < 0.f) relX = 0.f;
                        sf::String &lineStr = R[clickRow];
                        std::size_t n = lineStr.getSize();
                        float spaceAdvance = getGlyphAdvance(L' ', text.getCharacterSize());
                        float acc = 0.f;
                        std::size_t col = 0;
                        for (std::size_t i = 0;i < n; ++i) {
                            sf::Uint32 ch = lineStr[i];
                            float w = (ch==L'\t') ? spaceAdvance * tabDisplayWidth : getGlyphAdvance(ch, text.getCharacterSize());
                            if (acc + w*0.5f >= relX) { col = i; break; }
                            acc += w;
                            col = i+1;
                        }
                        cursor.row = clickRow;
                        cursor.col = col;
                        scrollOffset = rightScroll;
                        // 写回到该文件的持久状态，确保分屏各自显示独立光标/选区
                        if (rightIndex >= 0 && rightIndex < (int)openFiles.size()) {
                            openFiles[rightIndex].cursor = cursor;
                            openFiles[rightIndex].selection = selection;
                            openFiles[rightIndex].viewStart = rightScroll;
                        }
                        resetCursorBlink();
                        focused = true;
                        mouseDown = true;
                        selection.active = true;
                        selection.start = cursor;
                        selection.end = cursor;
                        return;
                    }
                }
            }
            // regular focus / cursor handling
            focused = box.getGlobalBounds().contains(m);
            if (focused) {
                // map mouse coords to content coords for cursor placement
                // we need pixel coords for setCursorFromMouse which expects pixels relative to window
                setCursorFromMouse(sf::Vector2f((float)mpixel.x, (float)mpixel.y));
                // 开始拖选
                mouseDown = true;
                selection.active = true;
                selection.start = cursor;
                selection.end = cursor;
            } else {
                selection.active = false;
                // clicking outside editor clears split pane focus
                splitPaneFocused = -1;
            }
        }

        if (event.getIf<sf::Event::MouseMoved>()) {
            auto mpos = sf::Mouse::getPosition(window);
            auto m = window.mapPixelToCoords(mpos);
            if (draggingSlider && activeSlider>=0) {
                // use track rect in world coords to compute pct
                sf::FloatRect track = sliderTrackRects[activeSlider];
                if (track.size.x <= 0) return;
                float pct = (m.x - track.position.x) / track.size.x;
                if (pct < 0.f) pct = 0.f;
                if (pct > 1.f) pct = 1.f;
                sliderValueInt[activeSlider] = (int)std::lround(pct * 255.f);
                updateRoleFromSliders(selectedColorRole);
                hexInputText = colorToHex(roleColors[selectedColorRole]);
                return;
            }
            // handle tab dragging visual feedback
            if (draggingTabIndex >= 0 && draggingTabIndex < (int)tabRects.size()) {
                float dx = m.x - (tabRects[draggingTabIndex].position.x + draggingTabOffset.x);
                float dy = m.y - (tabRects[draggingTabIndex].position.y + draggingTabOffset.y);
                if (!isTabDragging && (std::abs(dx) > 4.f || std::abs(dy) > 4.f)) {
                    isTabDragging = true;
                }
                if (isTabDragging) {
                    draggedTabPos = sf::Vector2f(m.x - draggingTabOffset.x, m.y - draggingTabOffset.y);
                    return; // while dragging a tab, don't do other mouse-move actions
                }
            }
            if (mouseDown && focused) {
                auto pm = sf::Mouse::getPosition(window);
                if (splitView && splitPaneFocused >= 0) {
                    sf::Vector2f mw = window.mapPixelToCoords(pm);
                    float paneLeft = box.getPosition().x + padding;
                    float paneTop = box.getPosition().y + padding + tabBarHeight;
                    float paneW = box.getSize().x - padding*2.f;
                    float gap = 6.f;
                    float leftW = std::max(10.f, paneW * 0.5f - gap);
                    // determine which pane
                    float localX = 0.f, localY = 0.f;
                    std::size_t linesPerPanel = std::max<std::size_t>(1, (std::size_t)std::max(1.f, std::floor((box.getSize().y - padding*2.f - tabBarHeight) / lineHeight)));
                    std::size_t scroll = 0;
                    sf::String *pline = nullptr;
                    if (splitPaneFocused == 0) {
                        localX = mw.x - (paneLeft + lineNumberWidth);
                        localY = mw.y - paneTop;
                        int leftIndex = (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex);
                        if (leftIndex >= 0 && leftIndex < (int)openFiles.size()) {
                            scroll = openFiles[leftIndex].viewStart;
                            pline = &openFiles[leftIndex].lines[0];
                        }
                    } else {
                        localX = mw.x - (paneLeft + leftW + gap + lineNumberWidth);
                        localY = mw.y - paneTop;
                        int rightIndex = (splitOtherIndex >= 0 ? splitOtherIndex : (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex));
                        if (rightIndex >= 0 && rightIndex < (int)openFiles.size()) {
                            scroll = openFiles[rightIndex].viewStart;
                            pline = &openFiles[rightIndex].lines[0];
                        }
                    }
                    if (localY < 0) localY = 0;
                    std::size_t clickRow = scroll + (std::size_t)std::floor(localY / lineHeight);
                    // clamp
                    int idxFile = (splitPaneFocused == 0 ? (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex) : (splitOtherIndex >= 0 ? splitOtherIndex : activeFileIndex));
                    if (idxFile >= 0 && idxFile < (int)openFiles.size()) {
                        auto &L = openFiles[idxFile].lines;
                        if (clickRow >= L.size()) clickRow = L.empty()?0:L.size()-1;
                        // compute column
                        float relX = localX + hScroll;
                        if (relX < 0.f) relX = 0.f;
                        sf::String &lineStr = L[clickRow];
                        std::size_t n = lineStr.getSize();
                        float spaceAdvance = getGlyphAdvance(L' ', text.getCharacterSize());
                        float acc = 0.f; std::size_t col = 0;
                        for (std::size_t i = 0;i < n; ++i) {
                            sf::Uint32 ch = lineStr[i];
                            float w = (ch==L'\t') ? spaceAdvance * tabDisplayWidth : getGlyphAdvance(ch, text.getCharacterSize());
                            if (acc + w*0.5f >= relX) { col = i; break; }
                            acc += w; col = i+1;
                        }
                        cursor.row = clickRow; cursor.col = col;
                        selection.end = cursor;
                        // 写回当前激活窗格对应文件的持久状态（拖拽选区时）
                        if (idxFile >= 0 && idxFile < (int)openFiles.size()) {
                            openFiles[idxFile].cursor = cursor;
                            openFiles[idxFile].selection = selection;
                            openFiles[idxFile].viewStart = scroll;
                        }
                    }
                } else {
                    setCursorFromMouse({(float)pm.x, (float)pm.y});
                    selection.end = cursor;
                }
            }
        }

        if (event.getIf<sf::Event::MouseButtonReleased>()) {
            if (mouseDown) {
                mouseDown = false;
            }
            // handle tab release: if we started dragging a tab, either drop to right pane or treat as click
            if (draggingTabIndex >= 0) {
                auto mpixel = sf::Mouse::getPosition(window);
                sf::Vector2f m = window.mapPixelToCoords(mpixel);
                // determine pane layout to detect drop into right pane
                float paneLeft = box.getPosition().x + padding;
                float paneTop = box.getPosition().y + padding + tabBarHeight;
                float paneW = box.getSize().x - padding*2.f;
                float gap = 6.f;
                float leftW = std::max(10.f, paneW * 0.5f - gap);
                float contentH = box.getSize().y - padding*2.f - tabBarHeight;
                // if dragged and released into right pane area, set splitOtherIndex
                if (isTabDragging && splitView && m.x >= paneLeft + leftW + gap && m.x < paneLeft + leftW + gap + (paneW - leftW - gap) && m.y >= paneTop && m.y < paneTop + contentH) {
                    splitOtherIndex = draggingTabIndex;
                    if (splitDiffEnabled && splitLeftIndex>=0 && splitOtherIndex>=0 && splitLeftIndex < (int)openFiles.size() && splitOtherIndex < (int)openFiles.size()) {
                        computeDiff(openFiles[splitLeftIndex].lines, openFiles[splitOtherIndex].lines, diffLeft, diffRight);
                    }
                } else {
                    // treat as simple click if not a drag
                    if (!isTabDragging) switchToOpenFile(draggingTabIndex);
                }
                // clear dragging state
                draggingTabIndex = -1;
                isTabDragging = false;
            }
            // stop slider drag
            if (draggingSlider) {
                draggingSlider = false;
                activeSlider = -1;
                saveColors();
            }
        }

        // Allow scrolling compile output even when editor not focused
        if (compileOutputVisible) {
            if (const auto* e = event.getIf<sf::Event::MouseWheelScrolled>()) {
                // compute overlay world rect (same as in draw)
                sf::Vector2u winSz = window.getSize();
                float pad = 10.f;
                float overlayW = std::max(1.f, (float)winSz.x - pad*2.f);
                float overlayH = std::min(200.f, (float)winSz.y * 0.25f);
                sf::Vector2i tlPixel((int)std::lround(pad), (int)std::lround((float)winSz.y - overlayH - pad));
                sf::Vector2i brPixel((int)std::lround(pad + overlayW), (int)std::lround((float)winSz.y - pad));
                sf::Vector2f tlWorld = window.mapPixelToCoords(tlPixel);
                sf::Vector2f brWorld = window.mapPixelToCoords(brPixel);
                sf::FloatRect overlayRect(tlWorld, brWorld - tlWorld);
                sf::Vector2i mp = sf::Mouse::getPosition(window);
                sf::Vector2f mw = window.mapPixelToCoords(mp);
                if (overlayRect.contains(mw)) {
                    // lines per page calculation
                    float lineSpacing = 16.f;
                    float textPadY = 6.f;
                    int linesPerPage = std::max(1, (int)std::floor((overlayRect.size.y - 2*textPadY) / lineSpacing));
                    if (e->delta > 0) {
                        // scroll up
                        if (compileOutputFirstLine > 0) compileOutputFirstLine = std::max<std::size_t>(0, compileOutputFirstLine - 1);
                    } else if (e->delta < 0) {
                        // scroll down
                        std::size_t maxFirst = 0;
                        if (compileOutputLines.size() > (std::size_t)linesPerPage) maxFirst = compileOutputLines.size() - linesPerPage;
                        compileOutputFirstLine = std::min(compileOutputFirstLine + 1, maxFirst);
                    }
                    return; // consume event
                }
            }
        }

        // Allow typing into hex input even when editor not focused
        if (settingsVisible && hexFocused) {
            if (const auto* te = event.getIf<sf::Event::TextEntered>()) {
                if (te->unicode == 8) {
                    if (!hexInputText.empty()) hexInputText.pop_back();
                } else if (te->unicode < 128) {
                    char ch = (char)te->unicode;
                    if ((ch>='0'&&ch<='9') || (ch>='a'&&ch<='f') || (ch>='A'&&ch<='F')) {
                        if (hexInputText.size() < 6) hexInputText.push_back(ch);
                    }
                }
                if (hexInputText.size() == 6) {
                    roleColors[selectedColorRole] = hexToColor(hexInputText);
                    updateSlidersFromRole(selectedColorRole);
                    saveColors();
                }
                return;
            }
            if (const auto* kp = event.getIf<sf::Event::KeyPressed>()) {
                if (kp->code == sf::Keyboard::Key::Backspace) {
                    if (!hexInputText.empty()) hexInputText.pop_back();
                    return;
                } else if (kp->code == sf::Keyboard::Key::Enter) {
                    if (hexInputText.size() == 6) {
                        roleColors[selectedColorRole] = hexToColor(hexInputText);
                        updateSlidersFromRole(selectedColorRole);
                        saveColors();
                    }
                    hexFocused = false;
                    return;
                } else if (kp->code == sf::Keyboard::Key::Escape) {
                    hexFocused = false;
                    return;
                }
            }
        }

        if (!focused && !(splitView && splitPaneFocused >= 0)) return;

        // If in split view and a pane has focus, ensure active file matches that pane
        if (splitView && splitPaneFocused >= 0) {
            int idx = (splitPaneFocused == 0) ? (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex) : (splitOtherIndex >= 0 ? splitOtherIndex : activeFileIndex);
            if (idx != activeFileIndex) focusFile(idx);
            focused = true;
        }

        if (const auto* e = event.getIf<sf::Event::MouseWheelScrolled>()) {
            if (e->wheel == sf::Mouse::Wheel::Vertical) {
                // if split view, scroll the pane under mouse; otherwise scroll active buffer
                if (splitView) {
                    sf::Vector2i mp = sf::Mouse::getPosition(window);
                    sf::Vector2f mw = window.mapPixelToCoords(mp);
                    float paneLeft = box.getPosition().x + padding;
                    float paneTop = box.getPosition().y + padding + tabBarHeight;
                    float paneW = box.getSize().x - padding*2.f;
                    float gap = 6.f;
                    float leftW = std::max(10.f, paneW * 0.5f - gap);
                    float contentH = box.getSize().y - padding*2.f - tabBarHeight;
                    // left pane
                    if (mw.x >= paneLeft && mw.x < paneLeft + leftW && mw.y >= paneTop && mw.y < paneTop + contentH) {
                        int li = (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex);
                        if (li >= 0 && li < (int)openFiles.size()) {
                            // 与右侧保持一致：wheel delta>0 -> 向上滚动（viewStart 减少）
                            std::size_t &vs = openFiles[li].viewStart;
                            if (e->delta > 0) { if (vs>0) vs = vs - 1; }
                            else { vs = std::min<std::size_t>(vs + 1, std::max<std::size_t>(0, openFiles[li].lines.size() > 0 ? openFiles[li].lines.size()-1 : 0)); }
                        }
                    } else if (mw.x >= paneLeft + leftW + gap && mw.x < paneLeft + leftW + gap + (paneW - leftW - gap) && mw.y >= paneTop && mw.y < paneTop + contentH) {
                        int ri = (splitOtherIndex >= 0 ? splitOtherIndex : (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex));
                        if (ri >= 0 && ri < (int)openFiles.size()) {
                            std::size_t &vs = openFiles[ri].viewStart;
                            if (e->delta > 0) { if (vs>0) vs = vs - 1; }
                            else { vs = std::min<std::size_t>(vs + 1, std::max<std::size_t>(0, openFiles[ri].lines.size() > 0 ? openFiles[ri].lines.size()-1 : 0)); }
                        }
                    } else {
                        scroll(-e->delta);
                    }
                } else {
                    scroll(-e->delta);
                }
            } else {
                scrollHoriz((int)(-e->delta*24));
            }
        }

        if (const auto* e = event.getIf<sf::Event::TextEntered>()) {
            if (settingsVisible && hexFocused) {
                // accept only hex chars and limit length to 6
                if (e->unicode == 8) {
                    if (!hexInputText.empty()) hexInputText.pop_back();
                } else if (e->unicode < 128) {
                    char ch = (char)e->unicode;
                    if ((ch>='0'&&ch<='9') || (ch>='a'&&ch<='f') || (ch>='A'&&ch<='F')) {
                        if (hexInputText.size() < 6) hexInputText.push_back(ch);
                    }
                }
                // if full 6 chars, apply
                if (hexInputText.size() == 6) {
                    roleColors[selectedColorRole] = hexToColor(hexInputText);
                    updateSlidersFromRole(selectedColorRole);
                    saveColors();
                }
            } else {
                if (e->unicode >= 32 && e->unicode != 127) {
                    pushUndo();
                    insertChar(e->unicode);
                    auto ch=e->unicode;
                    if(ch=='(') {
                        lines[cursor.row].insert(cursor.col, L')');
                    } else if(ch=='{') {
                        lines[cursor.row].insert(cursor.col, L'}');
                    } else if(ch=='[') {
                        lines[cursor.row].insert(cursor.col, L']');
                    } else if(ch=='"') {
                        lines[cursor.row].insert(cursor.col, L'"');
                    } else if(ch=='\'') {
                        lines[cursor.row].insert(cursor.col, L'\'');
                    } else if(subspace(lines[cursor.row].substring(0,cursor.col))==L"#include<"||subspace(lines[cursor.row].substring(0,cursor.col))==L"template<") {
                        lines[cursor.row].insert(cursor.col, L'>');
                    }
                }
            }
        }

        if (const auto* e = event.getIf<sf::Event::KeyPressed>()) {
            bool ctrl = e->control;
            bool shift = e->shift;
            // Ctrl+M: 切换左右分屏显示
            if (ctrl && e->code == sf::Keyboard::Key::M) {
                splitView = !splitView;
                if (splitView) {
                    splitLeftIndex = activeFileIndex;
                    if (openFiles.size() >= 2) splitOtherIndex = (splitLeftIndex + 1) % openFiles.size();
                    else splitOtherIndex = splitLeftIndex;
                    if (splitDiffEnabled) {
                        computeDiff(openFiles[splitLeftIndex].lines, openFiles[splitOtherIndex].lines, diffLeft, diffRight);
                    }
                } else {
                    // leaving split view, clear left/right indices and pane focus
                    splitLeftIndex = -1;
                    splitOtherIndex = -1;
                    splitPaneFocused = -1;
                }
                updateLayout();
                return;
            }
            // if hex input focused, handle edit keys here first
            if (settingsVisible && hexFocused) {
                if (e->code == sf::Keyboard::Key::Backspace) {
                    if (!hexInputText.empty()) hexInputText.pop_back();
                    return;
                } else if (e->code == sf::Keyboard::Key::Enter) {
                    if (hexInputText.size() == 6) {
                        roleColors[selectedColorRole] = hexToColor(hexInputText);
                        updateSlidersFromRole(selectedColorRole);
                        saveColors();
                    }
                    hexFocused = false;
                    return;
                } else if (e->code == sf::Keyboard::Key::Escape) {
                    hexFocused = false;
                    return;
                }
            }

            if (ctrl && e->code == sf::Keyboard::Key::A) selectAll();
            else if (ctrl && e->code == sf::Keyboard::Key::C) copy();
            else if (ctrl && e->code == sf::Keyboard::Key::V) paste();
            else if (ctrl && e->code == sf::Keyboard::Key::Z) undo();
            else if (ctrl && e->code == sf::Keyboard::Key::Y) redo();
            else if (ctrl && e->code == sf::Keyboard::Key::X) cut();
            else if (ctrl && e->code == sf::Keyboard::Key::E) {
                pushUndo();
                duplicateLineOrSelection();
            }
            else if (ctrl && e->code == sf::Keyboard::Key::D) {
                pushUndo();
                deleteLineOrSelection();
            }
            else if (ctrl && e->code == sf::Keyboard::Key::Slash) {
                pushUndo();
                // toggle comment on selected lines or current line
                toggleComment();
            }
            else if (e->code == sf::Keyboard::Key::Enter)
            {
                if(selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
                    pushUndo();
                    newline();
                }
                else
                {
                    std::string space_tab = "";
                    for (std::size_t i = 0; i < lines[cursor.row].getSize(); ++i) {
                        wchar_t ch = lines[cursor.row][i];
                        if (ch == L' ' || ch == L'\t') space_tab += (char)ch;
                        else break;
                    }
                    bool hasBrace = false;
                    if(lines[cursor.row].getSize()>0&&lines[cursor.row][cursor.col-1]==L'{')
                        space_tab += L'\t', hasBrace = true;
                    pushUndo();
                    newline();
                    for (char ch : space_tab) insertChar((sf::Uint32)ch);
                    auto tmp = cursor;
                    if(hasBrace && lines[cursor.row].getSize() > cursor.col && lines[cursor.row][cursor.col]==L'}') {
                        newline();
                        for (std::size_t i = 0; i + 1 < space_tab.size(); ++i)
                            insertChar((sf::Uint32)space_tab[i]);
                        cursor=tmp;
                    }
                }
            }
            else if (e->code == sf::Keyboard::Key::Delete) deleteForward();
            else if ((!ctrl && shift && e->code == sf::Keyboard::Key::Tab) || (ctrl && e->code == sf::Keyboard::Key::LBracket)) {
                // unindent selected lines or current line
                pushUndo();
                if(selection.active&&!(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
                    Cursor selStart = selection.start;
                    Cursor selEnd = selection.end;
                    if (selStart.row > selEnd.row || (selStart.row == selEnd.row && selStart.col > selEnd.col))
                        std::swap(selStart, selEnd);
                    for (std::size_t r = selStart.row; r <= selEnd.row; r++) {
                        if (lines[r].getSize() > 0 && lines[r][0] == L'\t') {
                            lines[r].erase(0, 1);
                        } else if (lines[r].getSize() > 1 && lines[r][0] == L' ' && lines[r][1] == L' ') {
                            lines[r].erase(0, 2);
                        }
                    }
                } else {
                    if (lines[cursor.row].getSize() > 0 && lines[cursor.row][0] == L'\t') {
                        lines[cursor.row].erase(0, 1);
                        if (cursor.col > 0) cursor.col = std::max<std::size_t>(0, cursor.col - 1);
                    } else if (lines[cursor.row].getSize() > 1 && lines[cursor.row][0] == L' ' && lines[cursor.row][1] == L' ') {
                        lines[cursor.row].erase(0, 2);
                        if (cursor.col > 1) cursor.col = std::max<std::size_t>(0, cursor.col - 2);
                    }
                }
                markModified();
                resetCursorBlink();
            }
            else if (!ctrl && e->code == sf::Keyboard::Key::Tab) printtab();
            else if (ctrl && e->code == sf::Keyboard::Key::RBracket) {
                // indent selected lines or current line
                pushUndo();
                if(selection.active&&!(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
                    Cursor selStart = selection.start;
                    Cursor selEnd = selection.end;
                    if (selStart.row > selEnd.row || (selStart.row == selEnd.row && selStart.col > selEnd.col))
                        std::swap(selStart, selEnd);
                    for (std::size_t r = selStart.row; r <= selEnd.row; r++) {
                        lines[r].insert(0, L"\t");
                    }
                } else {
                    lines[cursor.row].insert(0, L"\t");
                    cursor.col++;
                }
                markModified();
                resetCursorBlink();
            }
            else if (!ctrl && e->code == sf::Keyboard::Key::Backspace) backspace();
            else if (ctrl && e->code == sf::Keyboard::Key::Backspace)
            {
                if(selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
                    deleteSelection();
                } else {
                    // delete word before cursor
                    if (cursor.col == 0 && cursor.row == 0) return;
                    if (cursor.col == 0) {
                        // at start of line, join with previous line
                        cursor.row--;
                        cursor.col = lines[cursor.row].getSize();
                        lines[cursor.row] += lines[cursor.row + 1];
                        lines.erase(lines.begin() + cursor.row + 1);
                    } else {
                        // delete to previous word boundary
                        std::size_t origCol = cursor.col;
                        std::size_t newCol = origCol;
                        // skip any trailing spaces
                        while (newCol > 0 && (lines[cursor.row][newCol - 1] == L' ' || lines[cursor.row][newCol - 1] == L'\t'))
                            newCol--;
                        // then skip non-space characters
                        while (newCol > 0 && (lines[cursor.row][newCol - 1] != L' ' && lines[cursor.row][newCol - 1] != L'\t'))
                            newCol--;
                        lines[cursor.row].erase(newCol, origCol - newCol);
                        cursor.col = newCol;
                    }
                }
            }
            // 仅对方向键/导航键调用 moveCursor，避免按下 Ctrl/Shift 等修饰键意外清除选区
            else if (e->code == sf::Keyboard::Key::Left || e->code == sf::Keyboard::Key::Right
                  || e->code == sf::Keyboard::Key::Up || e->code == sf::Keyboard::Key::Down
                  || e->code == sf::Keyboard::Key::Home || e->code == sf::Keyboard::Key::End
                  || e->code == sf::Keyboard::Key::PageUp || e->code == sf::Keyboard::Key::PageDown) {
                moveCursor(e->code, shift);
            }
            // 文件 / 构建 快捷键
            if (ctrl && e->code == sf::Keyboard::Key::N) {
                createNewFile();
                return;
            }
            if (ctrl && e->code == sf::Keyboard::Key::W) {
                closeCurrentFile();
                return;
            }
            if (ctrl && e->code == sf::Keyboard::Key::Tab) {
                if (openFiles.size() > 1) {
                    int delta = shift ? -1 : 1;
                    if (splitView && splitPaneFocused == 0) {
                        // cycle left pane file, skipping the right pane's file to avoid duplication
                        int n = (int)openFiles.size();
                        int cur = (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex);
                        int cand = cur;
                        int attempts = n;
                        do {
                            cand = (cand + delta + n) % n;
                            attempts--;
                        } while (attempts > 0 && cand == splitOtherIndex);
                        if (cand != cur) {
                            splitLeftIndex = cand;
                            focusFile(splitLeftIndex);
                            if (splitDiffEnabled && splitLeftIndex>=0 && splitOtherIndex>=0) {
                                computeDiff(openFiles[splitLeftIndex].lines, openFiles[splitOtherIndex].lines, diffLeft, diffRight);
                            }
                        }
                    } else {
                        int newIdx = ((activeFileIndex + delta) % (int)openFiles.size() + (int)openFiles.size()) % (int)openFiles.size();
                        switchToOpenFile(newIdx);
                    }
                }
                return;
            }
            if (ctrl && e->code == sf::Keyboard::Key::S && shift) {
                saveAsDialog();
            } else if (ctrl && e->code == sf::Keyboard::Key::S) {
                save();
            } else if (ctrl && e->code == sf::Keyboard::Key::O) {
                openDialog();
            } else if (e->code == sf::Keyboard::Key::F9) {
                compileCurrentFile();
            } else if (e->code == sf::Keyboard::Key::F10) {
                runExecutable();
            } else if (e->code == sf::Keyboard::Key::F11) {
                if (compileCurrentFile()) runExecutable();
            }
        }
    }

    /* =================== 编辑逻辑 =================== */

    sf::String subspace(sf::String s)
    {
        sf::String res;
        for (auto ch : s) {
            if (!isspace(ch))
                res += ch;
        }
        return res;
    }

    void insertChar(sf::Uint32 ch) {
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            deleteSelection();
        }
        lines[cursor.row].insert(cursor.col, wchar_t(ch));
        cursor.col++;
        markModified();
        resetCursorBlink();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

    void newline() {
        pushUndo();
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            deleteSelection();
        }
        sf::String rest = lines[cursor.row].substring(cursor.col);
        lines[cursor.row] = lines[cursor.row].substring(0, cursor.col);
        lines.insert(lines.begin()+cursor.row+1, rest);
        cursor.row++;
        cursor.col = 0;
        markModified();
        resetCursorBlink();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

    void backspace() {

        pushUndo();
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            deleteSelection();
            return;
        }
        if (cursor.col > 0) {
            if(lines[cursor.row][cursor.col-1]==L'('||lines[cursor.row][cursor.col-1]==L'{'||
                                 lines[cursor.row][cursor.col-1]==L'['||
                                 lines[cursor.row][cursor.col-1]==L'"'||
                                 lines[cursor.row][cursor.col-1]==L'\'') {
                wchar_t matchChar = L')';
                if(lines[cursor.row][cursor.col-1]==L'{') matchChar = L'}';
                else if(lines[cursor.row][cursor.col-1]==L'[') matchChar = L']';
                else if(lines[cursor.row][cursor.col-1]==L'"') matchChar = L'"';
                else if(lines[cursor.row][cursor.col-1]==L'\'') matchChar = L'\'';
                if(lines[cursor.row][cursor.col]==matchChar) {
                    lines[cursor.row].erase(cursor.col,1);
                }
            }
            lines[cursor.row].erase(cursor.col-1, 1);
            cursor.col--;
        } else if (cursor.row > 0) {
            cursor.col = lines[cursor.row-1].getSize();
            lines[cursor.row-1] += lines[cursor.row];
            lines.erase(lines.begin()+cursor.row);
            cursor.row--;
        }
        markModified();
        resetCursorBlink();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

    void deleteForward() {
        pushUndo();
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            deleteSelection();
            return;
        }
        if (cursor.col < lines[cursor.row].getSize())
            lines[cursor.row].erase(cursor.col, 1);
        else if (cursor.row+1 < lines.size()) {
            lines[cursor.row] += lines[cursor.row+1];
            lines.erase(lines.begin()+cursor.row+1);
        }
        markModified();
        resetCursorBlink();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

	void printtab() {
        pushUndo();
        if(selection.active&&!(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            // tab for all selected lines
            Cursor selStart = selection.start;
            Cursor selEnd = selection.end;
            if (selStart.row > selEnd.row || (selStart.row == selEnd.row && selStart.col > selEnd.col))
                std::swap(selStart, selEnd);
            for (std::size_t r = selStart.row; r <= selEnd.row; r++) {
                lines[r].insert(0, L"\t");
            }
        }
        else {
		    lines[cursor.row].insert(cursor.col, L"\t");
		    cursor.col += 1;
        }
        markModified();
        resetCursorBlink();
	}

    /* =================== 光标 / 选择 =================== */

    void moveCursor(sf::Keyboard::Key key, bool shift) {
        resetCursorBlink();
        // For Up/Down navigation we preserve a desired visual column (in display cells)
        // For other navigation keys we clear it so subsequent Up/Down recomputes from new position
        bool isVert = (key == sf::Keyboard::Key::Up || key == sf::Keyboard::Key::Down);
        if (!isVert) desiredVisualCol = -1;
        if (shift && !selection.active) {
            selection.active = true;
            selection.start = cursor;
        }

        if (key == sf::Keyboard::Key::Left) {
            if (cursor.col > 0) cursor.col--;
            else if (cursor.row > 0) {
                cursor.row--;
                cursor.col = lines[cursor.row].getSize();
            }
        }
        else if (key == sf::Keyboard::Key::Right) {
            if (cursor.col < lines[cursor.row].getSize()) cursor.col++;
            else if (cursor.row+1 < lines.size()) {
                cursor.row++;
                cursor.col = 0;
            }
        }
        else if ((key == sf::Keyboard::Key::Up && cursor.row > 0) || (key == sf::Keyboard::Key::Down && cursor.row+1 < lines.size()))
        {
            // compute desired visual column if not set
            if (desiredVisualCol < 0) {
                // compute visual col of current cursor position
                int vc = 0;
                for (std::size_t i = 0; i < cursor.col && i < lines[cursor.row].getSize(); ++i) {
                    wchar_t ch = lines[cursor.row][i];
                    if (ch == L'\t') vc += tabDisplayWidth;
                    else if ((unsigned int)ch > 255) vc += 2; else vc += 1;
                }
                desiredVisualCol = vc;
            }
            // move row
            int targetRow = (key == sf::Keyboard::Key::Up) ? (int)cursor.row - 1 : (int)cursor.row + 1;
            if (targetRow < 0) targetRow = 0;
            // map desiredVisualCol to index in target row
            int acc = 0;
            std::size_t idx = 0;
            const auto &line = lines[targetRow];
            for (; idx < line.getSize(); ++idx) {
                wchar_t ch = line[idx];
                int w = (ch == L'\t') ? tabDisplayWidth : (((unsigned int)ch > 255) ? 2 : 1);
                if (acc + w > desiredVisualCol) break;
                acc += w;
            }
            cursor.row = (std::size_t)targetRow;
            cursor.col = idx;
            // clamp
            cursor.col = std::min(cursor.col, lines[cursor.row].getSize());
        }

        if (shift) selection.end = cursor;
        else selection.active = false;

        ensureCursorVisible();
    }

    /* =================== 剪贴板 =================== */

    void copy() {
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            sf::String s = getSelectedText();
            sf::Clipboard::setString(s);
            return;
        }
        sf::String all;
        for (std::size_t i=0;i<lines.size();++i) {
            all += lines[i];
            if (i+1<lines.size()) all+='\n';
        }
        sf::Clipboard::setString(all);
    }

    void cut() {
        pushUndo();
        copy();
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            deleteSelection();
            markModified();
        }
    }

    void paste() {
        pushUndo();
        sf::String clip = sf::Clipboard::getString();
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            deleteSelection();
        }
        for (auto ch:clip)
            ch=='\n'?newline():insertChar(ch);
        markModified();
        resetCursorBlink();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

    void deleteLineOrSelection() {
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            deleteSelection();
        } else {
            std::size_t curRow = cursor.row;
            lines.erase(lines.begin() + curRow);
            if (lines.empty()) {
                lines.emplace_back("");
                cursor = {0,0};
            } else {
                if (curRow >= lines.size()) curRow = lines.size() - 1;
                cursor = {curRow, std::min(cursor.col, lines[curRow].getSize())};
            }
        }
        markModified();
        resetCursorBlink();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

    void duplicateLineOrSelection() {
        if (selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            Cursor selStart = selection.start;
            Cursor selEnd = selection.end;
            if (selStart.row > selEnd.row || (selStart.row == selEnd.row && selStart.col > selEnd.col))
                std::swap(selStart, selEnd);
            std::size_t insertRow = selEnd.row + 1;
            for (std::size_t r = selStart.row; r <= selEnd.row; r++) {
                lines.insert(lines.begin() + insertRow, lines[r]);
                insertRow++;
            }
            cursor = {insertRow - 1, lines[insertRow - 1].getSize()};
        } else {
            std::size_t curRow = cursor.row;
            lines.insert(lines.begin() + curRow + 1, lines[curRow]);
            cursor = {curRow + 1, cursor.col};
        }
        markModified();
        resetCursorBlink();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

    void selectAll() {
        selection.active=true;
        selection.start={0,0};
        selection.end={lines.size()-1,lines.back().getSize()};
    }

    void toggleComment() {
        Cursor selStart = selection.start;
        Cursor selEnd = selection.end;
        if (!selection.active || (selection.start.row==selection.end.row && selection.start.col==selection.end.col)) {
            selStart = cursor;
            selEnd = cursor;
        }
        if (selStart.row > selEnd.row || (selStart.row == selEnd.row && selStart.col > selEnd.col))
            std::swap(selStart, selEnd);

        // check if all selected lines are commented
        bool allCommented = true;
        for (std::size_t r = selStart.row; r <= selEnd.row; r++) {
            sf::String line = lines[r];
            std::size_t firstNonSpace = 0;
            while (firstNonSpace < line.getSize() && isspace(line[firstNonSpace])) firstNonSpace++;
            if (!(firstNonSpace + 2 <= line.getSize() && line.substring(firstNonSpace, 2) == "//")) {
                allCommented = false;
                break;
            }
        }

        // toggle comment
        for (std::size_t r = selStart.row; r <= selEnd.row; r++) {
            sf::String& line = lines[r];
            std::size_t firstNonSpace = 0;
            while (firstNonSpace < line.getSize() && isspace(line[firstNonSpace])) firstNonSpace++;
            if (allCommented) {
                // uncomment
                if (firstNonSpace + 2 <= line.getSize() && line.substring(firstNonSpace, 2) == "//") {
                    line.erase(firstNonSpace); // remove '/'
                    line.erase(firstNonSpace); // remove second '/'
                }
            } else {
                // comment
                line.insert(firstNonSpace, L"//");
            }
        }
        markModified();
    }

    /* =================== Undo / Redo =================== */

    void pushUndo() {
        // push a snapshot of current state for undo; clear redo history
        auto& undoStack = openFiles[activeFileIndex].undoStack;
        auto& redoStack = openFiles[activeFileIndex].redoStack;
        undoStack.push({lines, lines, cursor, cursor});
        while(!redoStack.empty()) redoStack.pop();
    }

    void undo() {
        auto& undoStack = openFiles[activeFileIndex].undoStack;
        auto& redoStack = openFiles[activeFileIndex].redoStack;
        if (undoStack.empty()) return;
        // push current state for redo
        redoStack.push({lines, lines, cursor, cursor});
        // restore last undo snapshot
        lines = undoStack.top().before;
        cursor = undoStack.top().cursorBefore;
        undoStack.pop();
        clampCursor();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

    void redo() {
        auto& undoStack = openFiles[activeFileIndex].undoStack;
        auto& redoStack = openFiles[activeFileIndex].redoStack;
        if (redoStack.empty()) return;
        // push current state to undo stack
        undoStack.push({lines, lines, cursor, cursor});
        // restore redo snapshot
        lines = redoStack.top().before;
        cursor = redoStack.top().cursorBefore;
        redoStack.pop();
        clampCursor();
        ensureCursorVisible();
        syncCurrentToOpenFiles();
    }

    /* =================== 滚动 / 绘制 =================== */

    void scroll(int delta) {
        scrollOffset = std::clamp<int>(
            (int)scrollOffset + delta,
            0, std::max(0,(int)lines.size()-(int)visibleLines)
        );
    }

    void ensureCursorVisible() {
        // 计算当前内容区域可见行数（考虑 compileOutput 覆盖可能减少的高度）
        float contentH = box.getSize().y - padding*2.f - tabBarHeight;
        if (compileOutputVisible && hostWindow) {
            sf::Vector2u winSz = hostWindow->getSize();
            float pad = 10.f;
            float overlayHpx = std::min(200.f, (float)winSz.y * 0.25f);
            sf::Vector2f tlWorld = hostWindow->mapPixelToCoords({(int)std::lround(pad), (int)std::lround((float)winSz.y - overlayHpx - pad)});
            float overlayTopY = tlWorld.y;
            float boxTopY = box.getPosition().y;
            float boxBottomY = boxTopY + box.getSize().y;
            if (overlayTopY < boxBottomY) {
                float overlap = boxBottomY - std::max(overlayTopY, boxTopY);
                if (overlap > 0.f) contentH -= overlap;
            }
        }
        std::size_t visibleLinesNow = std::max<std::size_t>(1, (std::size_t)std::max(1.f, std::floor(contentH / lineHeight)));
        if (cursor.row < scrollOffset)
            scrollOffset = cursor.row;
        else if (cursor.row >= scrollOffset + visibleLinesNow)
            scrollOffset = cursor.row - visibleLinesNow + 1;
        // 确保光标在水平可视范围内（使用内容视图的局部坐标）
        sf::Text tmp = text;
        tmp.setString(lines[cursor.row]);
        tmp.setCharacterSize(text.getCharacterSize());
        // 在内容视图内，文本原点相对于内容左上角，所以把位置设为 -hScroll
        tmp.setPosition({-hScroll, 0});
        // compute pixel x of cursor using glyph advances and tab width
        float spaceAdvance = getGlyphAdvance(L' ', tmp.getCharacterSize());
        std::size_t n = lines[cursor.row].getSize();
        std::vector<float> charWidths(n);
        for (std::size_t i=0;i<n;++i) {
            sf::Uint32 ch = lines[cursor.row][i];
            if (ch == L'\t') charWidths[i] = spaceAdvance * tabDisplayWidth;
            else charWidths[i] = getGlyphAdvance(ch, tmp.getCharacterSize());
        }
        std::vector<float> offsets(n+1);
        offsets[0] = tmp.getPosition().x;
        for (std::size_t i=0;i<n;++i) offsets[i+1] = offsets[i] + charWidths[i];
        float px = offsets[std::min((std::size_t)cursor.col, n)];
        float contentW = box.getSize().x - lineNumberWidth - padding;
        float leftBound = hScrollMargin;
        float rightBound = std::max(0.f, contentW - hScrollMargin);
        if (px < leftBound) {
            hScroll = std::max(0.f, hScroll - (leftBound - px));
        }
        else if (px > rightBound) {
            hScroll += (px - rightBound);
            // clamp with dynamic margin
            float maxW = getMaxLineWidth();
            float allowedMax = std::max(0.f, maxW - contentW + hScrollMargin);
            if (hScroll > allowedMax) hScroll = allowedMax;
        }
    }

    void clampScroll() {
        scrollOffset = std::min(
            scrollOffset,
            std::max<std::size_t>(0,lines.size()-visibleLines)
        );
    }

    void clampCursor() {
        cursor.row = std::min(cursor.row, lines.size()-1);
        cursor.col = std::min(cursor.col, lines[cursor.row].getSize());
    }

    void update() {
        if (blinkClock.getElapsedTime().asSeconds()>0.5f) {
            cursorVisible=!cursorVisible;
            blinkClock.restart();
        }
    }

    void resetCursorBlink() {
        cursorVisible = true;
        blinkClock.restart();
    }

    // Tab width configuration
    void setTabWidth(int w) { tabDisplayWidth = std::max(1, w); }
    int getTabWidth() const { return tabDisplayWidth; }


    void drawCodeEditor(sf::RenderWindow &window, const sf::View &prevView, const std::vector<sf::String> &lines,
                        const Cursor *paneCursor = nullptr, const Selection *paneSelection = nullptr, std::size_t viewStartOverride = SIZE_MAX) {
        // 绘制文本与光标（在裁剪视图中）

		bool begincomment = false;

        std::size_t viewStart = (viewStartOverride==SIZE_MAX) ? scrollOffset : viewStartOverride;
        for (std::size_t i=0;i<visibleLines;i++) {
            std::size_t idx = i + viewStart;
            if (idx >= lines.size()) break;
            float yLocal = i*lineHeight;

            // 基准 text 用于计算字符位置（受水平偏移影响），位置为相对于内容区域左上角
            sf::Text base = text;
            base.setString(lines[idx]);
            base.setPosition({-hScroll, yLocal});

            const sf::Color selBg(0,0,128);
            const sf::Color selFg(255,255,255);
            // 辅助：绘制从列 a 到 b 的字符串（b 为 exclusive），若被选中则先绘制深蓝背景并用白字
            const Selection *useSel = paneSelection ? paneSelection : &selection;
            bool hasSel = useSel->active && !(useSel->start.row==useSel->end.row && useSel->start.col==useSel->end.col);
            Cursor s = useSel->start, e = useSel->end;
            if (s.row > e.row || (s.row==e.row && s.col>e.col)) std::swap(s,e);
            auto rangeSelected = [&](std::size_t a, std::size_t b)->bool{
                if (!hasSel) return false;
                if (idx < s.row || idx > e.row) return false;
                std::size_t sc = (idx==s.row) ? s.col : 0;
                std::size_t ec = (idx==e.row) ? e.col : lines[idx].getSize();
                return !(b<=sc || a>=ec);
            };
			
            // Precompute per-character widths for this line, respecting tabDisplayWidth
            float spaceAdvance = getGlyphAdvance(L' ', text.getCharacterSize());
            int n = lines[idx].getSize();
            std::vector<float> charWidths(n);
            for (std::size_t wi = 0; wi < (std::size_t)n; ++wi) {
                sf::Uint32 ch = lines[idx][wi];
                if (ch == L'\t') charWidths[wi] = spaceAdvance * tabDisplayWidth;
                else charWidths[wi] = getGlyphAdvance(ch, text.getCharacterSize());
            }
            std::vector<float> offsets(n+1);
            offsets[0] = base.getPosition().x;
            for (std::size_t oi = 0; oi < (std::size_t)n; ++oi) offsets[oi+1] = offsets[oi] + charWidths[oi];

            auto drwrange = [&](std::size_t a, std::size_t b, const sf::Color &fg, const sf::String &content){
                // draw background selection for full span
                float x1 = offsets[std::min(a, (std::size_t)n)];
                float x2 = offsets[std::min(b, (std::size_t)n)];
                float w = x2 - x1;
                if (w < 0) w = 0;
                if (rangeSelected(a,b)){
                    sf::RectangleShape r({w, lineHeight});
                    r.setPosition({x1, base.getPosition().y});
                    r.setFillColor(selBg);
                    window.draw(r);
                }
                // draw content by grouping contiguous chars that share the same font (CJK vs non-CJK)
                std::size_t len = content.getSize();
                std::size_t pos = 0;
                while (pos < len) {
                    sf::Uint32 ch = content[pos];
                    bool useCN = isWide(ch);
                    std::size_t runStart = pos;
                    while (pos < len && isWide(content[pos]) == useCN) pos++;
                    sf::String sub = content.substring(runStart, pos - runStart);
                    // compute x position for sub
                    float sx = offsets[std::min(a + runStart, (std::size_t)n)];
                    sf::Text t = text;
                    t.setString(sub);
                    t.setFillColor(rangeSelected(a + runStart, a + runStart + (pos - runStart)) ? selFg : fg);
                    // set font per run
                    if (useCN) t.setFont(fontCN); else t.setFont(font);
                    t.setCharacterSize(text.getCharacterSize());
                    t.setPosition({sx, base.getPosition().y});
                    window.draw(t);
                }
            };

            auto drawRange = [&](std::size_t a, std::size_t b, const sf::Color &fg, const sf::String &content){
                // draw by characters to keep alignment; content's string length should correspond to b-a
                if (b <= a) return;
                // rely on drwrange to split runs
                for(std::size_t i = 0; i < content.getSize(); i++) {
                    drwrange(a + i, a + i + 1, fg, content.substring(i, 1));
                }
            };

            // 规范化选择区并提供快速检测函数（基于列区间）
            

            // 判断标识符字符（字母、数字、下划线）
            auto isIdentChar = [](sf::Uint32 ch)->bool{
                if (ch=='_') return true;
                if (ch>=(sf::Uint32)'0' && ch<=(sf::Uint32)'9') return true;
                if (ch>=(sf::Uint32)'A' && ch<=(sf::Uint32)'Z') return true;
                if (ch>=(sf::Uint32)'a' && ch<=(sf::Uint32)'z') return true;
                return false;
            };

			// std::size_t n = lines[idx].getSize();
			// std::size_t posIndex = 0;
            const sf::Color &kwColor = roleColors[CR_Keyword];
            const sf::Color &kwtendColor = roleColors[CR_KeywordExt];
            const sf::Color &symbolColor = roleColors[CR_Symbol];
            const sf::Color &PreprocessorDirectivesColor = roleColors[CR_Preprocessor];
            const sf::Color &CommentColor = roleColors[CR_Comment];
            const sf::Color &StringColor = roleColors[CR_String];
            const sf::Color &HexColor = roleColors[CR_Hex];
            const sf::Color &OctColor = roleColors[CR_Oct];
            const sf::Color &FloatColor = roleColors[CR_Float];
            const sf::Color &IntColor = roleColors[CR_Int];
            const sf::Color &AfterDefinitionColor = roleColors[CR_AfterDefinition];
			
				
			if(begincomment)
			{
				sf::Text seg = text;
				seg.setString(lines[idx]);
				seg.setPosition(base.findCharacterPos(0));
				seg.setFillColor(CommentColor);
				if (rangeSelected(0, n)) seg.setFillColor(sf::Color(255,255,255));
				window.draw(seg);
			}
            // 为了保证注释优先正确识别，使用基于查找的分段流程：先定位注释边界，再对注释外的区间进行正常的词法分段。
            auto processNonCommentRange = [&](std::size_t start, std::size_t end){
                if (start >= end) return;
                // 此处复用之前的符号/关键字分段逻辑，针对 [start, end)
                std::vector<sf::String> syms;
                for (const auto &s: symbol) syms.push_back(s);
                std::sort(syms.begin(), syms.end(), [](const sf::String &a, const sf::String &b){ return a.getSize() > b.getSize(); });
                std::size_t p = start;
                while(lines[idx][p]==' ' || lines[idx][p]=='\t') p++; // 跳过前导空白
                drawRange(start, p, roleColors[CR_Text], lines[idx].substring(start, p - start));
                if(lines[idx][p]=='#')
				{
					// 预处理指令整行高亮
					if(lines[idx].find("#define")==0)
                    {
                        // define 之后继续正常分段
                        std::size_t defineEnd = p + 7;
                        drawRange(p, defineEnd, PreprocessorDirectivesColor, lines[idx].substring(p, defineEnd - p));
                        p = defineEnd;
                        drawRange(p, end, AfterDefinitionColor, lines[idx].substring(p, end - p));
                        return;
                    }
                    else
                    {
                        drawRange(p, end, PreprocessorDirectivesColor, lines[idx].substring(p, end - p));
					    return;
                    }
				}
				while (p < end) {
                    sf::Uint32 ch = lines[idx][p];

					if (ch == '\"' || ch == '\'') {
                        sf::Uint32 quoteType = ch;
                        std::size_t strStart = p;
                        p++; // 跳过起始引号
                        
                        while (p < end) {
                            sf::Uint32 cur = lines[idx][p];
                            // 遇到闭合引号，且前一个字符不是转义符 '\'
                            if (cur == quoteType && (p == 0 || lines[idx][p-1] != '\\')) {
                                p++; // 包含闭合引号
                                break;
                            }
                            p++;
                        }
                        
                        // 绘制字符串
                        sf::String strVal = lines[idx].substring(strStart, p - strStart);
                        drawRange(strStart, p, StringColor, strVal);
                        continue; // 跳转到下一个循环
                    }

                    if (!isIdentChar(ch)) {
                        // scan span of non-ident chars

                        std::size_t q = p+1;
                        while (q < end && !isIdentChar(lines[idx][q]) && lines[idx][q] != '\'' && lines[idx][q] != '\"') q++;
                        std::size_t k = p;
                        std::size_t simpleStart = k;
                        bool haveSimple = false;
                        while (k < q) {
                            bool matched = false;
                            for (const auto &sym: syms) {
                                std::size_t ssz = sym.getSize();
                                if (k + ssz <= q && lines[idx].substring(k, ssz) == sym) {
                                    if (haveSimple) {
                                        drawRange(simpleStart, k, roleColors[CR_Text], lines[idx].substring(simpleStart, k - simpleStart));
                                        haveSimple = false;
                                    }
                                    drawRange(k, k+ssz, symbolColor, sym);
                                    k += ssz;
                                    matched = true;
                                    break;
                                }
                            }
                            if (!matched) {
                                if (!haveSimple) { simpleStart = k; haveSimple = true; }
                                k++;
                            }
                        }
                        if (haveSimple) drawRange(simpleStart, k, roleColors[CR_Text], lines[idx].substring(simpleStart, k - simpleStart));
                        p = q;
                    } else {
                        // identifier
                        if(lines[idx][p]>='0' && lines[idx][p]<='9')
                        {
                            // 数字字面量
                            std::size_t q = p+1;
                            bool isHex = false,isOct = false,isFloat = false;
                            if(lines[idx][p]=='0'&&lines[idx][q]=='x') q++, isHex = true; // 16进制
                            if(lines[idx][p]=='0'&&lines[idx][q]>='0'&&lines[idx][q]<='7') isOct = true; // 8进制
                            while (q < end && ((lines[idx][q]>='0' && lines[idx][q]<='9')
                                || lines[idx][q]=='.' || lines[idx][q]=='e' 
                                || lines[idx][q]=='E' || (isHex && lines[idx][q]>='a' && lines[idx][q]<='f') || (isHex && lines[idx][q]>='A' && lines[idx][q]<='F')))
                                    isFloat |= (lines[idx][q]=='.' || lines[idx][q]=='e' || lines[idx][q]=='E'),q++;
                            if(isHex)
                                drawRange(p, q, HexColor , lines[idx].substring(p, q - p));
                            else if(isOct)
                                drawRange(p, q, OctColor , lines[idx].substring(p, q - p));
                            else if(isFloat)
                            {
                                if(q+1<end&&lines[idx].substring(q,2)=="lf")
                                    q+=2;
                                else if(q<end&&lines[idx][q]=='f')
                                    q++;
                                else if(q<end&&lines[idx][q]=='F')
                                    q++;
                                else if(q+1<end&&lines[idx].substring(q,2)=="Lf")
                                    q+=2;
                                else if(q+1<end&&lines[idx].substring(q,2)=="LF")
                                    q+=2;
                                else if(q<end&&lines[idx][q]=='l')
                                    q++;
                                else if(q<end&&lines[idx][q]=='L')
                                    q++;
                                drawRange(p, q, FloatColor , lines[idx].substring(p, q - p));
                            }
                            else
                            {
                                if(q+1<end&&lines[idx].substring(q,2)=="ll")
                                    q+=2;
                                else if(q<end&&lines[idx][q]=='l')
                                    q++;
                                else if(q<end&&lines[idx][q]=='u')
                                    q++;
                                else if(q+1<end&&lines[idx].substring(q,2)=="lu")
                                    q+=2;
                                else if(q+2<end&&lines[idx].substring(q,3)=="llu")
                                    q+=3;
                                else if(q+1<end&&lines[idx].substring(q,2)=="LU")
                                    q+=2;
                                else if(q+2<end&&lines[idx].substring(q,3)=="LLU")
                                    q+=3;
                                else if(q+1<end&&lines[idx].substring(q,2)=="ul")
                                    q+=2;
                                else if(q+1<end&&lines[idx].substring(q,2)=="UL")
                                    q+=2;
                                else if(q+2<end&&lines[idx].substring(q,3)=="uLL")
                                    q+=3;
                                else if(q+2<end&&lines[idx].substring(q,3)=="ull")
                                    q+=3;
                                else if(q+2<end&&lines[idx].substring(q,3)=="ULL")
                                    q+=3;
                                else if(q<end&&lines[idx][q]=='U')
                                    q++;
                                else if(q<end&&lines[idx][q]=='L')
                                    q++;
                                else if(q+1<end&&lines[idx].substring(q,2)=="LL")
                                    q+=2;
                                drawRange(p, q, IntColor , lines[idx].substring(p, q - p));
                            }
                            p = q;
                            continue;
                        }
                        std::size_t q = p+1;
                        while (q < end && isIdentChar(lines[idx][q])) q++;
                        sf::String token = lines[idx].substring(p, q-p);
                        if (highlight_word.count(token)) drawRange(p, q, kwColor, token);
                        else if (extend_highlight_word.count(token)) drawRange(p, q, kwtendColor, token);
                        else drawRange(p, q, roleColors[CR_Text], token);
                        p = q;
                    }
                }
            };

            std::size_t cursorPos = 0;
            while (cursorPos < (std::size_t)n) {
                if (begincomment) {
                    std::size_t commentEnd = lines[idx].find("*/", cursorPos);
                    if (commentEnd == sf::String::InvalidPos) {
                        // 整行为注释
                        drawRange(cursorPos, n, CommentColor, lines[idx].substring(cursorPos, n - cursorPos));
                        cursorPos = n;
                        begincomment = true;
                        break;
                    } else {
                        // 注释在本行结束
                        std::size_t len = commentEnd + 2 - cursorPos;
                        drawRange(cursorPos, cursorPos+len, CommentColor, lines[idx].substring(cursorPos, len));
                        cursorPos += len;
                        begincomment = false;
                        continue;
                    }
                }

                // 查找本行从 cursorPos 起的最近注释起始
                std::size_t linePos = lines[idx].find("//", cursorPos);
                std::size_t blockPos = lines[idx].find("/*", cursorPos);
                std::size_t nextCommentPos = sf::String::InvalidPos;
                bool isLineComment = false;
                if (linePos != sf::String::InvalidPos && (blockPos == sf::String::InvalidPos || linePos < blockPos)) {
                    nextCommentPos = linePos; isLineComment = true;
                } else if (blockPos != sf::String::InvalidPos) {
                    nextCommentPos = blockPos; isLineComment = false;
                }

                if (nextCommentPos == sf::String::InvalidPos) {
                    // 无注释，全部作为普通代码处理
                    processNonCommentRange(cursorPos, n);
                    cursorPos = n;
                } else {
                    // 先处理注释前的普通代码
                    if (nextCommentPos > cursorPos) processNonCommentRange(cursorPos, nextCommentPos);
                    if (isLineComment) {
                        drawRange(nextCommentPos, n, CommentColor, lines[idx].substring(nextCommentPos, n - nextCommentPos));
                        cursorPos = n;
                    } else {
                        // block comment
                        std::size_t commentEnd = lines[idx].find("*/", nextCommentPos+2);
                        if (commentEnd == sf::String::InvalidPos) {
                            drawRange(nextCommentPos, n, CommentColor, lines[idx].substring(nextCommentPos, n - nextCommentPos));
                            begincomment = true;
                            cursorPos = n;
                        } else {
                            std::size_t len = commentEnd + 2 - nextCommentPos;
                            drawRange(nextCommentPos, nextCommentPos+len, CommentColor, lines[idx].substring(nextCommentPos, len));
                            cursorPos = nextCommentPos + len;
                            begincomment = false;
                            // continue loop to find next comment in remainder
                        }
                    }
                }
            }

            const Cursor *useCur = paneCursor ? paneCursor : &cursor;
            if (cursorVisible && idx==useCur->row) {
                // position cursor using precomputed offsets if available
                float cx = base.getPosition().x;
                if (useCur->col <= (std::size_t)n) cx = offsets[std::min((std::size_t)useCur->col, (std::size_t)n)];
                float cy = base.getPosition().y;
                cursorShape.setPosition({cx, cy});
                window.draw(cursorShape);
            }
        }

        // 恢复视图
        window.setView(prevView);
    }

    

    void draw(sf::RenderWindow& window) {
        // ensure dynamic colors are applied (in case user edited them)
        box.setFillColor(roleColors[CR_Background]);
        text.setFillColor(roleColors[CR_Text]);
        lineNumText.setFillColor(roleColors[CR_LineNum]);
        window.draw(box);

        // draw tabs for open files at top-left inside the box
        tabRects.clear();
        tabIndexMap.clear();
        {
            float x = box.getPosition().x + padding;
            float y = box.getPosition().y + padding*0.5f;
            float tabH = std::max(18.f, text.getCharacterSize()+4.f);
            for (std::size_t i = 0; i < openFiles.size(); ++i) {
                // in split view, don't draw the file that's assigned to the right pane here
                if (splitView && (int)i == splitOtherIndex) continue;
                std::string name;
                if (!openFiles[i].path.empty()) name = std::filesystem::path(openFiles[i].path).filename().string();
                else name = "Untitled "+ std::to_string(openFiles[i].id);
                if (openFiles[i].modified) name = std::string("*") + name;
                sf::Text t(font, name, 14);
                t.setCharacterSize(14);
                auto b = t.getLocalBounds();
                float w = std::min(300.f, std::max(40.f, b.size.x + 20.f));
                if (x + w > box.getPosition().x + box.getSize().x - padding - compileToggleButtonW - 8.f) break; // avoid overlapping compile button
                sf::FloatRect r({x, y}, {w, tabH});
                sf::RectangleShape rr({r.size.x, r.size.y});
                rr.setPosition({r.position.x, r.position.y});
                rr.setFillColor((int)i == activeFileIndex ? roleColors[CR_FileTabsBackground_Active] : roleColors[CR_FileTabsBackground_Inactive]);
                rr.setOutlineThickness(1.f);
                rr.setOutlineColor(sf::Color(120,120,120));
                window.draw(rr);
                t.setPosition({r.position.x + 8.f, r.position.y + (r.size.y - b.size.y)/2.f - b.position.y});
                t.setFillColor(roleColors[CR_Text]);
                window.draw(t);
                tabRects.push_back(r);
                tabIndexMap.push_back((int)i);
                x += w + 6.f;
            }
        }

        // draw Settings button at top-right; internal controls are shown in a popup when open
        {
            sf::Vector2f compileBtnPos{
                box.getPosition().x + box.getSize().x - padding - compileToggleButtonW,
                box.getPosition().y + padding*0.5f
            };
            // Settings button sits left of the compile output button
            float settingsW = 80.f;
            sf::Vector2f settingsPos{ compileBtnPos.x - 6.f - settingsW, compileBtnPos.y };
                // draw a file tab for the right-side split file (if any) to the left of the settings button
                if (splitView && splitOtherIndex >= 0 && splitOtherIndex < (int)openFiles.size()) {
                    std::string name;
                    if (!openFiles[splitOtherIndex].path.empty()) name = std::filesystem::path(openFiles[splitOtherIndex].path).filename().string();
                    else name = "Untitled "+ std::to_string(openFiles[splitOtherIndex].id);
                    if (openFiles[splitOtherIndex].modified) name = std::string("*") + name;
                    sf::Text rt(font, name, 14);
                    rt.setCharacterSize(14);
                    auto rb = rt.getLocalBounds();
                    float rw = std::min(300.f, std::max(40.f, rb.size.x + 20.f));
                    float ry = box.getPosition().y + padding*0.5f;
                    float rx = settingsPos.x - 6.f - rw;
                    sf::FloatRect rrect({rx, ry}, {rw, std::max(18.f, text.getCharacterSize()+4.f)});
                    sf::RectangleShape rshape({rrect.size.x, rrect.size.y});
                    rshape.setPosition({rrect.position.x, rrect.position.y});
                    rshape.setFillColor((int)splitOtherIndex == activeFileIndex ? roleColors[CR_FileTabsBackground_Active] : roleColors[CR_FileTabsBackground_Inactive]);
                    rshape.setOutlineThickness(1.f);
                    rshape.setOutlineColor(sf::Color(120,120,120));
                    window.draw(rshape);
                    rt.setPosition({rrect.position.x + 8.f, rrect.position.y + (rrect.size.y - rb.size.y)/2.f - rb.position.y});
                    rt.setFillColor(roleColors[CR_Text]);
                    window.draw(rt);
                    rightTabRect = rrect;
                } else {
                    rightTabRect = sf::FloatRect();
                }
            sf::RectangleShape settingsBtn({settingsW, compileToggleButtonH});
            settingsBtn.setPosition(settingsPos);
            settingsBtn.setFillColor(roleColors[CR_Button]);
            settingsBtn.setOutlineThickness(1.f);
            settingsBtn.setOutlineColor(sf::Color::Black);
            window.draw(settingsBtn);
            sf::Text settingsText(font, "Settings", 12);
            settingsText.setCharacterSize(12);
            auto stb = settingsText.getLocalBounds();
            settingsText.setPosition(sf::Vector2f(settingsPos.x + (settingsW - stb.size.x)/2.f - stb.position.x, settingsPos.y + (compileToggleButtonH - stb.size.y)/2.f - stb.position.y));
            settingsText.setFillColor(roleColors[CR_ButtonText]);
            window.draw(settingsText);
            settingsBtnRect = sf::FloatRect(settingsPos, {settingsW, compileToggleButtonH});

            // draw compile output toggle to the right (same as before)
            sf::Vector2f btnPos = compileBtnPos;
            sf::RectangleShape btn({compileToggleButtonW, compileToggleButtonH});
            btn.setPosition(btnPos);
            btn.setFillColor(compileOutputVisible ? sf::Color(80,160,80) : roleColors[CR_Button]);
            btn.setOutlineThickness(1.f);
            btn.setOutlineColor(sf::Color::Black);
            window.draw(btn);
            sf::Text btnText(font, "", 12);
            btnText.setCharacterSize(12);
            btnText.setString(compileOutputVisible ? "Hide" : "Output");
            btnText.setFillColor(roleColors[CR_ButtonText]);
            auto tb = btnText.getLocalBounds();
            btnText.setPosition(sf::Vector2f(btnPos.x + (compileToggleButtonW - tb.size.x)/2.f - tb.position.x, btnPos.y + (compileToggleButtonH - tb.size.y)/2.f - tb.position.y));
            window.draw(btnText);

            // If settings visible, draw popup anchored below settings button
            if (settingsVisible) {
                float winW = 520.f;
                float winH = colorEditorVisible ? 546.f : 260.f;
                sf::Vector2f winPos{ settingsPos.x + settingsW - winW, settingsPos.y + compileToggleButtonH + 6.f };
                sf::RectangleShape win({winW, winH});
                win.setPosition(winPos);
                sf::Color winBg(roleColors[CR_SettingsBg].r, roleColors[CR_SettingsBg].g, roleColors[CR_SettingsBg].b, 255);
                win.setFillColor(winBg);
                win.setOutlineThickness(1.f);
                win.setOutlineColor(sf::Color::Black);
                window.draw(win);
                settingsWindowRect = sf::FloatRect(winPos, {winW, winH});

                float itemH = 28.f;
                float innerPad = 8.f;
                // Edit Default
                sf::Vector2f itPos{ winPos.x + innerPad, winPos.y + innerPad };
                sf::RectangleShape itBtn({winW - innerPad*2.f, itemH});
                itBtn.setPosition(itPos);
                itBtn.setFillColor(roleColors[CR_Button]);
                itBtn.setOutlineThickness(1.f);
                itBtn.setOutlineColor(sf::Color::Black);
                window.draw(itBtn);
                sf::Text itText(font, "Edit Default", 12);
                itText.setCharacterSize(12);
                auto itb = itText.getLocalBounds();
                itText.setPosition(sf::Vector2f(itPos.x + (itBtn.getSize().x - itb.size.x)/2.f - itb.position.x, itPos.y + (itemH - itb.size.y)/2.f - itb.position.y));
                itText.setFillColor(roleColors[CR_ButtonText]);
                window.draw(itText);
                settings_EditDefaultRect = sf::FloatRect(itPos, {itBtn.getSize().x, itBtn.getSize().y});

                // Edit Compile Command
                sf::Vector2f it2Pos{ winPos.x + innerPad, itPos.y + itemH + innerPad };
                sf::RectangleShape it2Btn({winW - innerPad*2.f, itemH});
                it2Btn.setPosition(it2Pos);
                it2Btn.setFillColor(roleColors[CR_Button]);
                it2Btn.setOutlineThickness(1.f);
                it2Btn.setOutlineColor(sf::Color::Black);
                window.draw(it2Btn);
                sf::Text it2Text(font, "Edit Compile Command", 12);
                it2Text.setCharacterSize(12);
                auto it2b = it2Text.getLocalBounds();
                it2Text.setPosition(sf::Vector2f(it2Pos.x + (it2Btn.getSize().x - it2b.size.x)/2.f - it2b.position.x, it2Pos.y + (itemH - it2b.size.y)/2.f - it2b.position.y));
                it2Text.setFillColor(roleColors[CR_ButtonText]);
                window.draw(it2Text);
                settings_EditCompileRect = sf::FloatRect(it2Pos, {it2Btn.getSize().x, it2Btn.getSize().y});

                // Toggle Output
                sf::Vector2f it3Pos{ winPos.x + innerPad, it2Pos.y + itemH + innerPad };
                sf::RectangleShape it3Btn({winW - innerPad*2.f, itemH});
                it3Btn.setPosition(it3Pos);
                it3Btn.setFillColor(compileOutputVisible ? sf::Color(180,230,180) : roleColors[CR_Button]);
                it3Btn.setOutlineThickness(1.f);
                it3Btn.setOutlineColor(sf::Color::Black);
                window.draw(it3Btn);
                sf::Text it3Text(font, compileOutputVisible ? "Output: Visible" : "Output: Hidden", 12);
                it3Text.setCharacterSize(12);
                auto it3b = it3Text.getLocalBounds();
                it3Text.setPosition(sf::Vector2f(it3Pos.x + (it3Btn.getSize().x - it3b.size.x)/2.f - it3b.position.x, it3Pos.y + (itemH - it3b.size.y)/2.f - it3b.position.y));
                it3Text.setFillColor(roleColors[CR_ButtonText]);
                window.draw(it3Text);
                settings_ToggleOutputRect = sf::FloatRect(it3Pos, {it3Btn.getSize().x, it3Btn.getSize().y});
                // Split Diff toggle
                sf::Vector2f itSplitPos{ winPos.x + innerPad, it3Pos.y + itemH + innerPad };
                sf::RectangleShape itSplitBtn({winW - innerPad*2.f, itemH});
                itSplitBtn.setPosition(itSplitPos);
                itSplitBtn.setFillColor(splitDiffEnabled ? sf::Color(200,200,120) : roleColors[CR_Button]);
                itSplitBtn.setOutlineThickness(1.f);
                itSplitBtn.setOutlineColor(sf::Color::Black);
                window.draw(itSplitBtn);
                sf::Text itSplitText(font, splitDiffEnabled ? "Split Diff: On" : "Split Diff: Off", 12);
                itSplitText.setCharacterSize(12);
                auto itSplitB = itSplitText.getLocalBounds();
                itSplitText.setPosition(sf::Vector2f(itSplitPos.x + (itSplitBtn.getSize().x - itSplitB.size.x)/2.f - itSplitB.position.x, itSplitPos.y + (itemH - itSplitB.size.y)/2.f - itSplitB.position.y));
                itSplitText.setFillColor(roleColors[CR_ButtonText]);
                window.draw(itSplitText);
                settings_SplitDiffRect = sf::FloatRect(itSplitPos, {itSplitBtn.getSize().x, itSplitBtn.getSize().y});
                // Custom Font EN button
                sf::Vector2f fontEnPos{ winPos.x + innerPad, itSplitPos.y + itemH + innerPad };
                sf::RectangleShape fontEnBtn({winW - innerPad*2.f, itemH});
                fontEnBtn.setPosition(fontEnPos);
                fontEnBtn.setFillColor(roleColors[CR_Button]);
                fontEnBtn.setOutlineThickness(1.f);
                fontEnBtn.setOutlineColor(sf::Color::Black);
                window.draw(fontEnBtn);
                std::string enLabel = std::string("Font (EN): ") + (fontPathEN.empty()?"(default)":fontPathEN);
                sf::Text fontEnText(font, enLabel, 12);
                fontEnText.setCharacterSize(12);
                fontEnText.setPosition(sf::Vector2f(fontEnPos.x + 6.f, fontEnPos.y + (itemH - fontEnText.getLocalBounds().size.y)/2.f - fontEnText.getLocalBounds().position.y));
                fontEnText.setFillColor(roleColors[CR_ButtonText]);
                window.draw(fontEnText);
                settings_CustomFontENRect = sf::FloatRect(fontEnPos, {fontEnBtn.getSize().x, fontEnBtn.getSize().y});

                // Custom Font CN button
                sf::Vector2f fontCnPos{ winPos.x + innerPad, fontEnPos.y + itemH + innerPad };
                sf::RectangleShape fontCnBtn({winW - innerPad*2.f, itemH});
                fontCnBtn.setPosition(fontCnPos);
                fontCnBtn.setFillColor(roleColors[CR_Button]);
                fontCnBtn.setOutlineThickness(1.f);
                fontCnBtn.setOutlineColor(sf::Color::Black);
                window.draw(fontCnBtn);
                auto string_to_wstring = [](const std::string& str) {
                    std::wstring wstr(str.begin(), str.end());
                    return wstr;
                };
                std::wstring cnLabel = std::wstring(L"字体（中文）: ") + (fontPathCN.empty()?L"(default)":string_to_wstring(fontPathCN));
                sf::Text fontCnText(fontCN, cnLabel, 12);
                fontCnText.setCharacterSize(12);
                fontCnText.setPosition(sf::Vector2f(fontCnPos.x + 6.f, fontCnPos.y + (itemH - fontCnText.getLocalBounds().size.y)/2.f - fontCnText.getLocalBounds().position.y));
                fontCnText.setFillColor(roleColors[CR_ButtonText]);
                window.draw(fontCnText);
                settings_CustomFontCNRect = sf::FloatRect(fontCnPos, {fontCnBtn.getSize().x, fontCnBtn.getSize().y});
                
                // Color tab button (shows/hides color editor inside settings)
                sf::Vector2f colorTabPos{winPos.x + innerPad, fontCnPos.y + itemH + innerPad};
                sf::RectangleShape colorTab({winW - innerPad*2.f, itemH});
                colorTab.setPosition(colorTabPos);
                colorTab.setFillColor(colorEditorVisible ? roleColors[CR_Button] : sf::Color(220,220,220));
                colorTab.setOutlineThickness(1.f);
                colorTab.setOutlineColor(sf::Color::Black);
                window.draw(colorTab);
                sf::Text colorTabText(font, "Colors", 12);
                colorTabText.setCharacterSize(12);
                colorTabText.setPosition(sf::Vector2f(colorTabPos.x + (colorTab.getSize().x - colorTabText.getLocalBounds().size.x)/2.f - colorTabText.getLocalBounds().position.x, colorTabPos.y + (itemH - colorTabText.getLocalBounds().size.y)/2.f - colorTabText.getLocalBounds().position.y));
                colorTabText.setFillColor(roleColors[CR_ButtonText]);
                window.draw(colorTabText);
                settings_ColorTabRect = sf::FloatRect(colorTabPos, {colorTab.getSize().x, colorTab.getSize().y});

                // Tab width control
                sf::Vector2f tabPos{ winPos.x + innerPad, colorTabPos.y + itemH + innerPad };
                float smallW = 28.f;
                sf::RectangleShape decBtn({smallW, itemH});
                decBtn.setPosition(tabPos);
                decBtn.setFillColor(roleColors[CR_Button]);
                decBtn.setOutlineThickness(1.f);
                decBtn.setOutlineColor(sf::Color::Black);
                window.draw(decBtn);
                sf::Text decText(font, "-", 16);
                decText.setCharacterSize(16);
                auto dtb = decText.getLocalBounds();
                decText.setPosition(sf::Vector2f(tabPos.x + (smallW - dtb.size.x)/2.f - dtb.position.x, tabPos.y + (itemH - dtb.size.y)/2.f - dtb.position.y - 2.f));
                decText.setFillColor(roleColors[CR_ButtonText]);
                window.draw(decText);
                settings_TabDecRect = sf::FloatRect(tabPos, {smallW, itemH});

                sf::Text tabLabel(font, std::string("Tab Width: ") + std::to_string(tabDisplayWidth), 12);
                tabLabel.setCharacterSize(12);
                auto tlb = tabLabel.getLocalBounds();
                tabLabel.setPosition(sf::Vector2f(tabPos.x + smallW + 8.f, tabPos.y + (itemH - tlb.size.y)/2.f - tlb.position.y));
                tabLabel.setFillColor(roleColors[CR_SettingsText]);
                window.draw(tabLabel);

                sf::RectangleShape incBtn({smallW, itemH});
                incBtn.setPosition(sf::Vector2f(winPos.x + winW - innerPad - smallW, tabPos.y));
                incBtn.setFillColor(roleColors[CR_Button]);
                incBtn.setOutlineThickness(1.f);
                incBtn.setOutlineColor(sf::Color::Black);
                window.draw(incBtn);
                sf::Text incText(font, "+", 16);
                incText.setCharacterSize(16);
                auto itb2 = incText.getLocalBounds();
                incText.setPosition(sf::Vector2f(incBtn.getPosition().x + (smallW - itb2.size.x)/2.f - itb2.position.x, incBtn.getPosition().y + (itemH - itb2.size.y)/2.f - itb2.position.y - 2.f));
                incText.setFillColor(roleColors[CR_ButtonText]);
                window.draw(incText);
                settings_TabIncRect = sf::FloatRect(incBtn.getPosition(), incBtn.getSize());
                if (colorEditorVisible) {
                // Draw color swatches
                float swX = winPos.x + innerPad;
                float swY = tabPos.y + itemH + innerPad;
                float swH = 20.f;
                float swW = 20.f;
                colorSwatchRects.clear();
                // draw swatches in two columns
                int cols = 2;
                int rows = (CR_COUNT + cols - 1) / cols;
                float colGap = 160.f; // space between columns (including label area)
                for (int ri = 0; ri < CR_COUNT; ++ri) {
                    int col = ri / rows; // 0..cols-1
                    int row = ri % rows;
                    float cx = swX + col * colGap;
                    float yy = swY + row * (swH + 6.f);
                    sf::RectangleShape sbox({swW, swH});
                    sbox.setPosition({cx, yy});
                    sbox.setFillColor(roleColors[ri]);
                    sbox.setOutlineThickness(1.f);
                    sbox.setOutlineColor(ri==selectedColorRole?sf::Color::Yellow:sf::Color::Black);
                    window.draw(sbox);
                    float lblX = cx + swW + 8.f;
                    sf::Text lbl(font, roleNames[ri], 12);
                    lbl.setCharacterSize(12);
                    lbl.setPosition({lblX, yy});
                    lbl.setFillColor(roleColors[CR_SettingsText]);
                    window.draw(lbl);
                    colorSwatchRects.push_back(sf::FloatRect({cx, yy}, {swW, swH}));
                }

                // draw editor panel on right side
                float editorX = winPos.x + winW/2.f + 8.f;
                float editorY = swY;
                float editorW = winW/2.f - innerPad*2.f - 8.f;
                // role name
                sf::Text roleTitle(font, std::string("Edit: ") + roleNames[selectedColorRole], 14);
                roleTitle.setCharacterSize(14);
                roleTitle.setPosition({editorX, editorY});
                roleTitle.setFillColor(roleColors[CR_SettingsText]);
                window.draw(roleTitle);
                editorY += 26.f;

                // sliders
                float trackH = 12.f;
                float trackW = editorW - 80.f;
                float trackX = editorX + 60.f;
                for (int si=0; si<3; ++si) {
                    // label
                    const char *lab = si==0?"R":(si==1?"G":"B");
                    sf::Text l(font, lab, 12);
                    l.setCharacterSize(12);
                    l.setPosition({editorX, editorY});
                    l.setFillColor(roleColors[CR_SettingsText]);
                    window.draw(l);

                    // track background
                        sf::RectangleShape track({trackW, trackH});
                        track.setPosition({trackX, editorY + 6.f});
                    track.setFillColor(sf::Color(200,200,200));
                    track.setOutlineThickness(1.f);
                    track.setOutlineColor(sf::Color::Black);
                    window.draw(track);
                    // filled portion
                    float pct = (float)sliderValueInt[si] / 255.f;
                    sf::RectangleShape fill({trackW * pct, trackH});
                    fill.setPosition(track.getPosition());
                    // color tint per channel
                    sf::Color tint = sf::Color::Black;
                    if (si==0) tint = sf::Color(sliderValueInt[0],0,0);
                    else if (si==1) tint = sf::Color(0,sliderValueInt[1],0);
                    else tint = sf::Color(0,0,sliderValueInt[2]);
                    fill.setFillColor(tint);
                    window.draw(fill);

                    // slider knob rect (for hit testing)
                    float kx = track.getPosition().x + trackW * pct - 6.f;
                    float ky = track.getPosition().y - 6.f;
                    sf::RectangleShape knob({12.f, trackH + 12.f});
                    knob.setPosition({kx, ky});
                    knob.setFillColor(sf::Color(240,240,240));
                    knob.setOutlineThickness(1.f);
                    knob.setOutlineColor(sf::Color::Black);
                    window.draw(knob);
                    sliderRects[si] = sf::FloatRect(knob.getPosition(), knob.getSize());
                    sliderTrackRects[si] = sf::FloatRect(track.getPosition(), track.getSize());

                    // numeric value
                    sf::Text vtxt(font, std::to_string(sliderValueInt[si]), 12);
                    vtxt.setCharacterSize(12);
                    vtxt.setPosition({trackX + trackW + 8.f, editorY + 2.f});
                    vtxt.setFillColor(roleColors[CR_SettingsText]);
                    window.draw(vtxt);

                    editorY += 30.f;
                }

                // hex input
                sf::Text hexLabel(font, "Hex:", 12);
                hexLabel.setCharacterSize(12);
                hexLabel.setPosition({editorX, editorY});
                hexLabel.setFillColor(roleColors[CR_SettingsText]);
                window.draw(hexLabel);
                hexInputRect = sf::FloatRect({editorX + 40.f, editorY}, {120.f, 20.f});
                sf::RectangleShape hexBox({hexInputRect.size.x, hexInputRect.size.y});
                hexBox.setPosition({hexInputRect.position.x, hexInputRect.position.y});
                hexBox.setFillColor(sf::Color::White);
                hexBox.setOutlineThickness(1.f);
                hexBox.setOutlineColor(hexFocused?sf::Color::Yellow:sf::Color::Black);
                window.draw(hexBox);
                sf::Text hexText(font, hexInputText, 12);
                hexText.setCharacterSize(12);
                hexText.setPosition({hexInputRect.position.x + 4.f, hexInputRect.position.y + 2.f});
                hexText.setFillColor(roleColors[CR_SettingsText]);
                window.draw(hexText);
                }
            } else {
                settingsWindowRect = sf::FloatRect();
            }
        }

        // 先在默认视图绘制行号（不受水平滚动影响）
        // draw floating dragged tab (on top) if any
        if (isTabDragging && draggingTabIndex >= 0 && draggingTabIndex < (int)openFiles.size()) {
            std::string name;
            if (!openFiles[draggingTabIndex].path.empty()) name = std::filesystem::path(openFiles[draggingTabIndex].path).filename().string();
            else name = "Untitled "+ std::to_string(openFiles[draggingTabIndex].id);
            if (openFiles[draggingTabIndex].modified) name = std::string("*") + name;
            sf::Text dt(font, name, 14);
            dt.setCharacterSize(14);
            auto db = dt.getLocalBounds();
            float dw = std::min(300.f, std::max(40.f, db.size.x + 20.f));
            float dh = std::max(18.f, text.getCharacterSize()+4.f);
            sf::RectangleShape dshape({dw, dh});
            dshape.setPosition(draggedTabPos);
            dshape.setFillColor(roleColors[CR_FileTabsBackground_Active]);
            dshape.setOutlineThickness(1.f);
            dshape.setOutlineColor(sf::Color(120,120,120));
            window.draw(dshape);
            dt.setPosition({draggedTabPos.x + 8.f, draggedTabPos.y + (dh - db.size.y)/2.f - db.position.y});
            dt.setFillColor(roleColors[CR_Text]);
            window.draw(dt);
        }
        if (!splitView) {
            for (std::size_t i=0;i<visibleLines;i++) {
                std::size_t idx=i+scrollOffset;
                if (idx>=lines.size()) break;
                float y = box.getPosition().y + padding + tabBarHeight + i*lineHeight;
                lineNumText.setString(std::to_string(idx+1));
                // map pixel coordinates to world coordinates to handle window scaling / DPI correctly
                sf::Vector2f pixelPos(
                    lround(box.getPosition().x + 4.f),
                    lround(y)
                );
                // sf::Vector2f worldPos = window.mapPixelToCoords(pixelPos);
                lineNumText.setPosition(pixelPos);
                window.draw(lineNumText);
            }
        }
            else
            {
                // 分屏模式下，行号显示为两侧行号（各自使用各自 openFiles 的 viewStart）
                float gap = 6.f;
                float paneLeft = box.getPosition().x + padding;
                float paneTop = box.getPosition().y + padding + tabBarHeight;
                float paneW = box.getSize().x - padding*2.f;
                float leftW = std::max(10.f, paneW * 0.5f - gap);
                float rightW = std::max(10.f, paneW - leftW - gap);
                int leftIndex = (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex);
                int rightIndex = (splitOtherIndex >= 0 ? splitOtherIndex : (splitLeftIndex >= 0 ? splitLeftIndex : activeFileIndex));
                // 左侧行号
                if (leftIndex >= 0 && leftIndex < (int)openFiles.size()) {
                    std::size_t vs = openFiles[leftIndex].viewStart;
                    auto &L = openFiles[leftIndex].lines;
                    for (std::size_t i=0;i<visibleLines;i++) {
                        std::size_t idx = i + vs;
                        if (idx>=L.size()) break;
                        float y = paneTop + i*lineHeight;
                        lineNumText.setString(std::to_string(idx+1));
                        sf::Vector2f pixelPos(
                            lround(paneLeft + 4.f),
                            lround(y)
                        );
                        lineNumText.setPosition(pixelPos);
                        window.draw(lineNumText);
                    }
                }
                // 右侧行号
                if (rightIndex >= 0 && rightIndex < (int)openFiles.size()) {
                    std::size_t vsr = openFiles[rightIndex].viewStart;
                    auto &R = openFiles[rightIndex].lines;
                    for (std::size_t i=0;i<visibleLines;i++) {
                        std::size_t idx = i + vsr;
                        if (idx>=R.size()) break;
                        float y = paneTop + i*lineHeight;
                        lineNumText.setString(std::to_string(idx+1));
                        sf::Vector2f pixelPos(
                            lround(paneLeft + leftW + gap + 4.f),
                            lround(y)
                        );
                        lineNumText.setPosition(pixelPos);
                        window.draw(lineNumText);
                    }
                }
            }

        // 创建裁剪视图，使文本不会绘制到行号区域
        auto prevView = window.getView();
        float contentLeft = box.getPosition().x + lineNumberWidth;
        float contentTop = box.getPosition().y + padding + tabBarHeight;
        float contentW = box.getSize().x - lineNumberWidth - padding;
        float contentH = box.getSize().y - padding*2.f - tabBarHeight;
        if (contentW < 1.f) contentW = 1.f;
        if (contentH < 1.f) contentH = 1.f;
        // 如果启用分屏，绘制左右两侧视图并设置标志
        bool splitRendered = false;
        if (splitView) {
            // 准备左右面板宽度和视口
            float gap = 6.f;
            float paneLeft = box.getPosition().x + padding;
            float paneTop = contentTop;
            float paneW = box.getSize().x - padding*2.f;
            float leftW = std::max(10.f, paneW * 0.5f - gap);
            float rightW = std::max(10.f, paneW - leftW - gap);
            sf::Vector2u winSz = window.getSize();
            // 给行号留出空间
            leftW -= lineNumberWidth;
            rightW -= lineNumberWidth;

            // 左视图：如果指定了 splitLeftIndex，则显示该文件；否则显示其他文件的聚合预览
            std::vector<sf::String> leftLines;
            if (splitLeftIndex >= 0 && splitLeftIndex < (int)openFiles.size()) {
                leftLines = openFiles[splitLeftIndex].lines;
            } else {
                auto activeFile = openFiles[activeFileIndex];
                if (openFiles.size() > 1) {
                    for (std::size_t i = 0; i < openFiles.size(); ++i) {
                        if (i == (std::size_t)activeFileIndex) continue;
                        for (const auto& line : openFiles[i].lines) {
                            leftLines.push_back(line);
                        }
                        // 添加空行分隔不同文件
                        leftLines.push_back("");
                    }
                } else {
                    // 只有一个文件时，左侧显示空白
                    leftLines.push_back("// No previous file to show");
                }
            }

            sf::View leftView;
            leftView.setCenter({leftW/2.f, contentH/2.f});
            leftView.setSize({leftW, contentH});
            if (winSz.x > 0 && winSz.y > 0) {
                leftView.setViewport(sf::FloatRect(
                    {(paneLeft+lineNumberWidth) / (float)winSz.x,
                     paneTop / (float)winSz.y},
                    {leftW / (float)winSz.x,
                     contentH / (float)winSz.y}
                ));
            }
            window.setView(leftView);

            if (splitLeftIndex >= 0 && splitLeftIndex < (int)openFiles.size()) {
                drawCodeEditor(window, prevView, leftLines, &openFiles[splitLeftIndex].cursor, &openFiles[splitLeftIndex].selection, openFiles[splitLeftIndex].viewStart);
            } else {
                drawCodeEditor(window, prevView, leftLines);
            }

            // 右视图（当前文件或 splitOtherIndex 指定的文件）
            int rightIndex = (splitOtherIndex >= 0 ? splitOtherIndex : activeFileIndex);
            std::vector<sf::String> rightLines;
            if (rightIndex >= 0 && rightIndex < (int)openFiles.size()) rightLines = openFiles[rightIndex].lines;
            sf::View rightView;
            rightView.setCenter({rightW/2.f, contentH/2.f});
            rightView.setSize({rightW, contentH});
            if (winSz.x > 0 && winSz.y > 0) {
                rightView.setViewport(sf::FloatRect(
                    {(paneLeft + leftW + gap + lineNumberWidth * 2) / (float)winSz.x,
                     paneTop / (float)winSz.y},
                    {rightW / (float)winSz.x,
                     contentH / (float)winSz.y}
                ));
            }
            window.setView(rightView);
            
            if (rightIndex >= 0 && rightIndex < (int)openFiles.size()) {
                drawCodeEditor(window, prevView, rightLines, &openFiles[rightIndex].cursor, &openFiles[rightIndex].selection, openFiles[rightIndex].viewStart);
            } else {
                drawCodeEditor(window, prevView, rightLines);
            }

            // 恢复视图并标记已完成分屏渲染（后续保留 overlay 绘制）
            // window.setView(prevView);
            // 若需要绘制其他 overlay（如 settings），后续代码仍会执行
            splitRendered = true;
        }
        if (!splitRendered) {
            sf::View contentView;
            // 使用内容区域的局部坐标系：世界坐标 0..contentW x 0..contentH
            contentView.setCenter({contentW/2.f, contentH/2.f});
            contentView.setSize({contentW, contentH});
            sf::Vector2u winSz = window.getSize();
            if (winSz.x > 0 && winSz.y > 0) {
                contentView.setViewport(sf::FloatRect(
                    {contentLeft / (float)winSz.x,
                    contentTop / (float)winSz.y},
                    {contentW / (float)winSz.x,
                    contentH / (float)winSz.y}
                )); 
            }
            window.setView(contentView);
            drawCodeEditor(window, prevView, lines);
            // window.setView(prevView);
        }
        // Ensure settings popup is drawn on top (opaque) so underlying text cannot show through
        if (settingsVisible && settingsWindowRect.size.x > 0 && settingsWindowRect.size.y > 0) {
            sf::FloatRect wr = settingsWindowRect;
            sf::Vector2f winPos{wr.position.x, wr.position.y};
            float winW2 = wr.size.x;
            float winH2 = wr.size.y;
            sf::RectangleShape winTop({winW2, winH2});
            winTop.setPosition(winPos);
            sf::Color winBg2(roleColors[CR_SettingsBg].r, roleColors[CR_SettingsBg].g, roleColors[CR_SettingsBg].b, 255);
            winTop.setFillColor(winBg2);
            winTop.setOutlineThickness(1.f);
            winTop.setOutlineColor(sf::Color::Black);
            window.draw(winTop);

            float itemH2 = 28.f;
            float innerPad2 = 8.f;
            // Color tab
            sf::Vector2f colorTabPos{winPos.x + innerPad2, winPos.y + innerPad2 + itemH2*3.f + innerPad2*3.f};
            sf::RectangleShape colorTab2({winW2 - innerPad2*2.f, itemH2});
            colorTab2.setPosition(colorTabPos);
            colorTab2.setFillColor(colorEditorVisible ? roleColors[CR_Button] : sf::Color(220,220,220));
            colorTab2.setOutlineThickness(1.f);
            colorTab2.setOutlineColor(sf::Color::Black);
            window.draw(colorTab2);
            sf::Text colorTabText2(font, "Colors", 12);
            colorTabText2.setCharacterSize(12);
            colorTabText2.setPosition(sf::Vector2f(colorTabPos.x + (colorTab2.getSize().x - colorTabText2.getLocalBounds().size.x)/2.f - colorTabText2.getLocalBounds().position.x, colorTabPos.y + (itemH2 - colorTabText2.getLocalBounds().size.y)/2.f - colorTabText2.getLocalBounds().position.y));
            colorTabText2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(colorTabText2);

            // Main buttons
            sf::Vector2f itPos2{ winPos.x + innerPad2, winPos.y + innerPad2 };
            sf::RectangleShape itBtn2({winW2 - innerPad2*2.f, itemH2});
            itBtn2.setPosition(itPos2);
            itBtn2.setFillColor(roleColors[CR_Button]);
            itBtn2.setOutlineThickness(1.f);
            itBtn2.setOutlineColor(sf::Color::Black);
            window.draw(itBtn2);
            sf::Text itText2(font, "Edit Default", 12);
            itText2.setCharacterSize(12);
            auto itb2b = itText2.getLocalBounds();
            itText2.setPosition(sf::Vector2f(itPos2.x + (itBtn2.getSize().x - itb2b.size.x)/2.f - itb2b.position.x, itPos2.y + (itemH2 - itb2b.size.y)/2.f - itb2b.position.y));
            itText2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(itText2);

            sf::Vector2f it2Pos2{ winPos.x + innerPad2, itPos2.y + itemH2 + innerPad2 };
            sf::RectangleShape it2Btn2({winW2 - innerPad2*2.f, itemH2});
            it2Btn2.setPosition(it2Pos2);
            it2Btn2.setFillColor(roleColors[CR_Button]);
            it2Btn2.setOutlineThickness(1.f);
            it2Btn2.setOutlineColor(sf::Color::Black);
            window.draw(it2Btn2);
            sf::Text it2Text2(font, "Edit Compile Command", 12);
            it2Text2.setCharacterSize(12);
            auto it2b2b = it2Text2.getLocalBounds();
            it2Text2.setPosition(sf::Vector2f(it2Pos2.x + (it2Btn2.getSize().x - it2b2b.size.x)/2.f - it2b2b.position.x, it2Pos2.y + (itemH2 - it2b2b.size.y)/2.f - it2b2b.position.y));
            it2Text2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(it2Text2);

            sf::Vector2f it3Pos2{ winPos.x + innerPad2, it2Pos2.y + itemH2 + innerPad2 };
            sf::RectangleShape it3Btn2({winW2 - innerPad2*2.f, itemH2});
            it3Btn2.setPosition(it3Pos2);
            it3Btn2.setFillColor(compileOutputVisible ? sf::Color(180,230,180) : roleColors[CR_Button]);
            it3Btn2.setOutlineThickness(1.f);
            it3Btn2.setOutlineColor(sf::Color::Black);
            window.draw(it3Btn2);
            sf::Text it3Text2(font, compileOutputVisible ? "Output: Visible" : "Output: Hidden", 12);
            it3Text2.setCharacterSize(12);
            auto it3b2b = it3Text2.getLocalBounds();
            it3Text2.setPosition(sf::Vector2f(it3Pos2.x + (it3Btn2.getSize().x - it3b2b.size.x)/2.f - it3b2b.position.x, it3Pos2.y + (itemH2 - it3b2b.size.y)/2.f - it3b2b.position.y));
            it3Text2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(it3Text2);
            // Split Diff toggle (overlay)
            sf::Vector2f itSplitPos2{ winPos.x + innerPad2, it3Pos2.y + itemH2 + innerPad2 };
            sf::RectangleShape itSplitBtn2({winW2 - innerPad2*2.f, itemH2});
            itSplitBtn2.setPosition(itSplitPos2);
            itSplitBtn2.setFillColor(splitDiffEnabled ? sf::Color(200,200,120) : roleColors[CR_Button]);
            itSplitBtn2.setOutlineThickness(1.f);
            itSplitBtn2.setOutlineColor(sf::Color::Black);
            window.draw(itSplitBtn2);
            sf::Text itSplitText2(font, splitDiffEnabled ? "Split Diff: On" : "Split Diff: Off", 12);
            itSplitText2.setCharacterSize(12);
            auto itSplitB2 = itSplitText2.getLocalBounds();
            itSplitText2.setPosition(sf::Vector2f(itSplitPos2.x + (itSplitBtn2.getSize().x - itSplitB2.size.x)/2.f - itSplitB2.position.x, itSplitPos2.y + (itemH2 - itSplitB2.size.y)/2.f - itSplitB2.position.y));
            itSplitText2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(itSplitText2);
            settings_SplitDiffRect = sf::FloatRect(itSplitPos2, {itSplitBtn2.getSize().x, itSplitBtn2.getSize().y});

            // Custom Font EN button (overlay)
            sf::Vector2f fontEnPos2{ winPos.x + innerPad2, itSplitPos2.y + itemH2 + innerPad2 };
            sf::RectangleShape fontEnBtn2({winW2 - innerPad2*2.f, itemH2});
            fontEnBtn2.setPosition(fontEnPos2);
            fontEnBtn2.setFillColor(roleColors[CR_Button]);
            fontEnBtn2.setOutlineThickness(1.f);
            fontEnBtn2.setOutlineColor(sf::Color::Black);
            window.draw(fontEnBtn2);
            std::string enLabel2 = std::string("Font (EN): ") + (fontPathEN.empty()?"(default)":fontPathEN);
            sf::Text fontEnText2(font, enLabel2, 12);
            fontEnText2.setCharacterSize(12);
            fontEnText2.setPosition(sf::Vector2f(fontEnPos2.x + 6.f, fontEnPos2.y + (itemH2 - fontEnText2.getLocalBounds().size.y)/2.f - fontEnText2.getLocalBounds().position.y));
            fontEnText2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(fontEnText2);
            settings_CustomFontENRect = sf::FloatRect(fontEnPos2, {fontEnBtn2.getSize().x, fontEnBtn2.getSize().y});

            // Custom Font CN button (overlay)
            sf::Vector2f fontCnPos2{ winPos.x + innerPad2, fontEnPos2.y + itemH2 + innerPad2 };
            sf::RectangleShape fontCnBtn2({winW2 - innerPad2*2.f, itemH2});
            fontCnBtn2.setPosition(fontCnPos2);
            fontCnBtn2.setFillColor(roleColors[CR_Button]);
            fontCnBtn2.setOutlineThickness(1.f);
            fontCnBtn2.setOutlineColor(sf::Color::Black);
            window.draw(fontCnBtn2);
            std::wstring cnLabel2 = std::wstring(L"字体（中文）: ") + (fontPathCN.empty()?L"(default)":std::wstring(fontPathCN.begin(), fontPathCN.end()));
            sf::Text fontCnText2(fontCN, cnLabel2, 12);
            fontCnText2.setCharacterSize(12);
            fontCnText2.setPosition(sf::Vector2f(fontCnPos2.x + 6.f, fontCnPos2.y + (itemH2 - fontCnText2.getLocalBounds().size.y)/2.f - fontCnText2.getLocalBounds().position.y));
            fontCnText2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(fontCnText2);
            settings_CustomFontCNRect = sf::FloatRect(fontCnPos2, {fontCnBtn2.getSize().x, fontCnBtn2.getSize().y});

            // Color Tab Button
            sf::Vector2f ColorTabPos{winPos.x + innerPad2, fontCnPos2.y + itemH2 + innerPad2};
            sf::RectangleShape ColorTab2({winW2 - innerPad2*2.f, itemH2});
            ColorTab2.setPosition(ColorTabPos);
            ColorTab2.setFillColor(roleColors[CR_Button]);
            ColorTab2.setOutlineThickness(1.f);
            ColorTab2.setOutlineColor(sf::Color::Black);
            window.draw(ColorTab2);
            sf::Text ColorTabText2(font, "Colors", 16);
            ColorTabText2.setCharacterSize(12);
            auto ctb = ColorTabText2.getLocalBounds();
            ColorTabText2.setPosition(sf::Vector2f(ColorTabPos.x + (ColorTab2.getSize().x - ctb.size.x)/2.f - ctb.position.x, ColorTabPos.y + (itemH2 - ctb.size.y)/2.f - ctb.position.y));
            ColorTabText2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(ColorTabText2);

            // Tab width controls
            sf::Vector2f tabPos2{ winPos.x + innerPad2, ColorTabPos.y + itemH2 + innerPad2 };
            float smallW2 = 28.f;
            sf::RectangleShape decBtn2({smallW2, itemH2});
            decBtn2.setPosition(tabPos2);
            decBtn2.setFillColor(roleColors[CR_Button]);
            decBtn2.setOutlineThickness(1.f);
            decBtn2.setOutlineColor(sf::Color::Black);
            window.draw(decBtn2);
            sf::Text decText2(font, "-", 16);
            decText2.setCharacterSize(16);
            auto dtb2b = decText2.getLocalBounds();
            decText2.setPosition(sf::Vector2f(tabPos2.x + (smallW2 - dtb2b.size.x)/2.f - dtb2b.position.x, tabPos2.y + (itemH2 - dtb2b.size.y)/2.f - dtb2b.position.y - 2.f));
            decText2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(decText2);

            sf::Text tabLabel2(font, std::string("Tab Width: ") + std::to_string(tabDisplayWidth), 12);
            tabLabel2.setCharacterSize(12);
            tabLabel2.setPosition(sf::Vector2f(tabPos2.x + smallW2 + 8.f, tabPos2.y + (itemH2 - tabLabel2.getLocalBounds().size.y)/2.f - tabLabel2.getLocalBounds().position.y));
            tabLabel2.setFillColor(roleColors[CR_SettingsText]);
            window.draw(tabLabel2);

            sf::RectangleShape incBtn2({smallW2, itemH2});
            incBtn2.setPosition(sf::Vector2f(winPos.x + winW2 - innerPad2 - smallW2, tabPos2.y));
            incBtn2.setFillColor(roleColors[CR_Button]);
            incBtn2.setOutlineThickness(1.f);
            incBtn2.setOutlineColor(sf::Color::Black);
            window.draw(incBtn2);
            sf::Text incText2(font, "+", 16);
            incText2.setCharacterSize(16);
            auto itb2b2 = incText2.getLocalBounds();
            incText2.setPosition(sf::Vector2f(incBtn2.getPosition().x + (smallW2 - itb2b2.size.x)/2.f - itb2b2.position.x, incBtn2.getPosition().y + (itemH2 - itb2b2.size.y)/2.f - itb2b2.position.y - 2.f));
            incText2.setFillColor(roleColors[CR_ButtonText]);
            window.draw(incText2);

            // Color editor area
            if (colorEditorVisible) {
                float swX2 = winPos.x + innerPad2;
                float swY2 = tabPos2.y + itemH2 + innerPad2;
                float swH2 = 20.f;
                float swW2 = 20.f;
                int cols2 = 2;
                int rows2 = (CR_COUNT + cols2 - 1) / cols2;
                float colGap2 = 160.f;
                for (int ri = 0; ri < CR_COUNT; ++ri) {
                    int col = ri / rows2;
                    int row = ri % rows2;
                    float cx2 = swX2 + col * colGap2;
                    float yy2 = swY2 + row * (swH2 + 6.f);
                    sf::RectangleShape sbox2({swW2, swH2});
                    sbox2.setPosition({cx2, yy2});
                    sbox2.setFillColor(roleColors[ri]);
                    sbox2.setOutlineThickness(1.f);
                    sbox2.setOutlineColor(ri==selectedColorRole?sf::Color::Yellow:sf::Color::Black);
                    window.draw(sbox2);
                    float lblX2 = cx2 + swW2 + 8.f;
                    sf::Text lbl2(font, roleNames[ri], 12);
                    lbl2.setCharacterSize(12);
                    lbl2.setPosition({lblX2, yy2});
                    lbl2.setFillColor(roleColors[CR_SettingsText]);
                    window.draw(lbl2);
                }
                // editor minimal: role title and hex
                float editorX2 = winPos.x + winW2/2.f + 8.f;
                float editorY2 = swY2;
                sf::Text roleTitle2(font, std::string("Edit: ") + roleNames[selectedColorRole], 14);
                roleTitle2.setCharacterSize(14);
                roleTitle2.setPosition({editorX2, editorY2});
                roleTitle2.setFillColor(roleColors[CR_SettingsText]);
                window.draw(roleTitle2);
                editorY2 += 26.f;
                sf::Text hexLabel2(font, "Hex:", 12);
                hexLabel2.setCharacterSize(12);
                hexLabel2.setPosition({editorX2, editorY2});
                hexLabel2.setFillColor(roleColors[CR_SettingsText]);
                window.draw(hexLabel2);
                sf::FloatRect hexInputRect2({editorX2 + 40.f, editorY2}, {120.f, 20.f});
                // keep member rect in sync with overlay input so clicks hit correctly
                hexInputRect = hexInputRect2;
                sf::RectangleShape hexBox2({hexInputRect2.size.x, hexInputRect2.size.y});
                hexBox2.setPosition({hexInputRect2.position.x, hexInputRect2.position.y});
                hexBox2.setFillColor(sf::Color::White);
                hexBox2.setOutlineThickness(1.f);
                hexBox2.setOutlineColor(hexFocused?sf::Color::Yellow:sf::Color::Black);
                window.draw(hexBox2);
                sf::Text hexText2(font, hexInputText, 12);
                hexText2.setCharacterSize(12);
                hexText2.setPosition({hexInputRect2.position.x + 4.f, hexInputRect2.position.y + 2.f});
                hexText2.setFillColor(roleColors[CR_SettingsText]);
                window.draw(hexText2);
                // draw sliders in overlay as well so knobs remain visible above content
                float editorW2 = winW2/2.f - innerPad2*2.f - 8.f;
                float EditorX2 = winPos.x + winW2/2.f + 8.f;
                float EditorY2 = hexInputRect2.position.y + hexInputRect2.size.y + 8.f;
                float trackH2 = 12.f;
                float trackW2 = editorW2 - 80.f;
                float trackX2 = EditorX2 + 60.f;
                for (int si=0; si<3; ++si) {
                    const char *lab2 = si==0?"R":(si==1?"G":"B");
                    sf::Text l2(font, lab2, 12);
                    l2.setCharacterSize(12);
                    l2.setPosition({EditorX2, EditorY2});
                    l2.setFillColor(roleColors[CR_SettingsText]);
                    window.draw(l2);

                    sf::RectangleShape track2({trackW2, trackH2});
                    track2.setPosition({trackX2, EditorY2 + 6.f});
                    track2.setFillColor(sf::Color(200,200,200));
                    track2.setOutlineThickness(1.f);
                    track2.setOutlineColor(sf::Color::Black);
                    window.draw(track2);

                    float pct2 = (float)sliderValueInt[si] / 255.f;
                    sf::RectangleShape fill2({trackW2 * pct2, trackH2});
                    fill2.setPosition(track2.getPosition());
                    sf::Color tint2 = sf::Color::Black;
                    if (si==0) tint2 = sf::Color(sliderValueInt[0],0,0);
                    else if (si==1) tint2 = sf::Color(0,sliderValueInt[1],0);
                    else tint2 = sf::Color(0,0,sliderValueInt[2]);
                    fill2.setFillColor(tint2);
                    window.draw(fill2);

                    float kx2 = track2.getPosition().x + trackW2 * pct2 - 6.f;
                    float ky2 = track2.getPosition().y - 6.f;
                    sf::RectangleShape knob2({12.f, trackH2 + 12.f});
                    knob2.setPosition({kx2, ky2});
                    knob2.setFillColor(sf::Color(240,240,240));
                    knob2.setOutlineThickness(1.f);
                    knob2.setOutlineColor(sf::Color::Black);
                    window.draw(knob2);
                    sliderRects[si] = sf::FloatRect(knob2.getPosition(), knob2.getSize());
                    sliderTrackRects[si] = sf::FloatRect(track2.getPosition(), track2.getSize());

                    sf::Text vtxt2(font, std::to_string(sliderValueInt[si]), 12);
                    vtxt2.setCharacterSize(12);
                    vtxt2.setPosition({trackX2 + trackW2 + 8.f, EditorY2 + 2.f});
                    vtxt2.setFillColor(roleColors[CR_SettingsText]);
                    window.draw(vtxt2);

                    EditorY2 += 30.f;
                }
            }
        }
        // draw compile output overlay if available
        if (compileOutputVisible && !compileOutputLines.empty()) {
            // compute overlay rectangle in pixel space then map to world coords
            sf::Vector2u winSz = window.getSize();
            float pad = 10.f;
            float overlayW = std::max(1.f, (float)winSz.x - pad*2.f);
            float overlayH = std::min(200.f, (float)winSz.y * 0.25f);
            sf::Vector2i tlPixel((int)std::lround(pad), (int)std::lround((float)winSz.y - overlayH - pad));
            sf::Vector2i brPixel((int)std::lround(pad + overlayW), (int)std::lround((float)winSz.y - pad));
            sf::Vector2f tlWorld = window.mapPixelToCoords(tlPixel);
            sf::Vector2f brWorld = window.mapPixelToCoords(brPixel);
            sf::RectangleShape overlay;
            overlay.setPosition(tlWorld);
            overlay.setSize(brWorld - tlWorld);
            overlay.setFillColor(roleColors[CR_CompileBg]);
            overlay.setOutlineThickness(1.f);
            sf::Color outlineC = roleColors[CR_CompileFg]; outlineC.a = 100;
            overlay.setOutlineColor(outlineC);
            window.draw(overlay);

            sf::Text outText(font, "", 14);
            outText.setCharacterSize(14);
            outText.setFillColor(roleColors[CR_CompileFg]);
            float lineSpacing = 16.f;
            // text positions in world coords relative to overlay
            float textPadX = 6.f;
            float textPadY = 6.f;
            sf::Vector2f overlayPos = overlay.getPosition();
            int linesPerPage = std::max(1, (int)std::floor((overlay.getSize().y - 2*textPadY) / lineSpacing));
            std::size_t start = std::min(compileOutputFirstLine, compileOutputLines.size());
            for (int i=0;i<linesPerPage && (start + (std::size_t)i) < compileOutputLines.size(); ++i) {
                outText.setString(compileOutputLines[start + i]);
                outText.setPosition({overlayPos.x + textPadX, overlayPos.y + textPadY + i*lineSpacing});
                window.draw(outText);
            }
        }
    }
	
private:
    void setCursorFromMouse(sf::Vector2f m) {
        float y=m.y-box.getPosition().y-padding-tabBarHeight;
        cursor.row=std::min(
            scrollOffset+(std::size_t)(y/lineHeight),
            lines.size()-1
        );

        sf::Text temp = text;
        temp.setString(lines[cursor.row]);
        temp.setCharacterSize(text.getCharacterSize());
        sf::Vector2f basePos = {box.getPosition().x + lineNumberWidth - hScroll,
                                box.getPosition().y + padding + tabBarHeight + (cursor.row - scrollOffset)*lineHeight};
        temp.setPosition(basePos);
        // compute widths for this line using glyph advances and tab width
        std::size_t n = lines[cursor.row].getSize();
        float spaceAdvance = getGlyphAdvance(L' ', temp.getCharacterSize());
        std::vector<float> charWidths(n);
        for (std::size_t i=0;i<n;++i) {
            sf::Uint32 ch = lines[cursor.row][i];
            if (ch == L'\t') charWidths[i] = spaceAdvance * tabDisplayWidth;
            else charWidths[i] = getGlyphAdvance(ch, temp.getCharacterSize());
        }
        std::vector<float> offsets(n+1);
        offsets[0] = basePos.x;
        for (std::size_t i=0;i<n;++i) offsets[i+1] = offsets[i] + charWidths[i];
        // find index where mouse.x is before next char boundary
        cursor.col = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (m.x < offsets[i+1]) { cursor.col = (int)i; break; }
        }
        resetCursorBlink();
    }

    // ---------------- File IO / Build helpers ----------------
#ifdef _WIN32
    static std::string openFileDialogWin() {
        OPENFILENAMEA ofn;
        CHAR szFile[MAX_PATH] = {0};
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = "All\0*.*\0C++ Source\0*.cpp;*.hpp;*.h\0C Source\0*.c;*.h\0Text\0*.txt\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) return std::string(szFile);
        return std::string();
    }

    static std::string saveFileDialogWin(const char *defExt = "cpp") {
        OPENFILENAMEA ofn;
        CHAR szFile[MAX_PATH] = {0};
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = "All\0*.*\0Text\0*.txt\0C++ Source\0*.cpp;*.hpp;*.h\0";
        ofn.lpstrDefExt = defExt;
        ofn.Flags = OFN_PATHMUSTEXIST;
        if (GetSaveFileNameA(&ofn)) return std::string(szFile);
        return std::string();
    }
#endif

    bool saveToFilePath(const std::string &path) {
        try {
            std::ofstream ofs(path, std::ios::binary);
            if (!ofs) return false;
            for (std::size_t i=0;i<lines.size();++i) {
                std::string s = utf8_encode_string(lines[i]);
                ofs << s;
                if (i+1<lines.size()) ofs << '\n';
            }
            ofs.close();
            // update current file path and the openFiles entry
            currentFile = path;
            if (activeFileIndex >= 0 && activeFileIndex < (int)openFiles.size())
                openFiles[activeFileIndex].path = path;
            if (activeFileIndex >= 0 && activeFileIndex < (int)openFiles.size())
                openFiles[activeFileIndex].modified = false;
            updateWindowTitle();
            return true;
        } catch(...) { return false; }
    }

    bool openFromFilePath(const std::string &path) {
        try {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) return false;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            // parse into vector<sf::String>
            std::vector<sf::String> parsed;
            std::size_t pos = 0;
            while (pos <= content.size()) {
                std::size_t nl = content.find('\n', pos);
                if (nl == std::string::npos) nl = content.size();
                std::string chunk = content.substr(pos, nl - pos);
                parsed.emplace_back(utf8_decode_string(chunk));
                if (nl == content.size()) break;
                pos = nl + 1;
            }
            if (parsed.empty()) parsed.emplace_back("");
            // add as new open file and switch to it
            OpenFile f;
            f.path = path;
            f.lines = parsed;
            f.modified = false;
            f.cursor = {0,0};
            f.selection = Selection();
            f.viewStart = 0;
            openFiles.push_back(std::move(f));
            switchToOpenFile((int)openFiles.size()-1);
            return true;
        } catch(...) { return false; }
    }

    void save() {
        if (currentFile.empty()) {
            saveAsDialog();
            return;
        }
        saveToFilePath(currentFile);
    }

    void saveAsDialog() {
#ifdef _WIN32
        std::string p = saveFileDialogWin("cpp");
        if (!p.empty()) saveToFilePath(p);
#else
        // fallback
        saveToFilePath("untitled.cpp");
#endif
    }

    void openDialog() {
#ifdef _WIN32
        std::string p = openFileDialogWin();
        if (!p.empty()) openFromFilePath(p);
#endif
    }

    bool compileCurrentFile() {
        compileOutputLines.clear();
            // prompt save if modified
        if(promptSaveIfModified(activeFileIndex,1)>=2)
            return false;
            // sync current edits back
        try{
            syncCurrentToOpenFiles();
            std::filesystem::path p(currentFile);
            auto exe = p.replace_extension(".exe").string();
            std::string cmd;
            wchar_t strSelf[MAX_PATH];
            memset(strSelf, 0, sizeof(strSelf));
            GetModuleFileNameW(NULL, strSelf, sizeof(strSelf));
            std::wstring WcurrentDir = strSelf;
            for(auto it=WcurrentDir.rbegin(); it!=WcurrentDir.rend(); ++it) {
                if (*it == L'\\' || *it == L'/') {
                    WcurrentDir.erase(it.base()-1, WcurrentDir.end());
                    break;
                }
            }
            WcurrentDir += L"\\";
            std::string currentDir = utf8_encode_string(WcurrentDir);
            std::ifstream ifs((currentDir+"compile.txt").c_str());
            if (ifs) {
                std::string line;
                if (std::getline(ifs, line)) {
                    if (!line.empty()) compileTemplate = line;
                }
            }
            if (!compileTemplate.empty()) {
                cmd = compileTemplate;
                auto replaceAll = [](std::string str, const std::string &from, const std::string &to) {
                    std::size_t start_pos = 0;
                    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
                        str.replace(start_pos, from.length(), to);
                        start_pos += to.length();
                    }
                    return str;
                };
                cmd=replaceAll(cmd, "%FILE%", currentFile);
                cmd=replaceAll(cmd, "%OUT%", exe);
            } else {
                std::ostringstream ss;
                ss << gppPath << " " << extraFlags << " \"" << currentFile << "\" -o \"" << exe << "\"";
                cmd = ss.str();
            }

            cmd = "title Compiling " + currentFile + " ... & " + cmd;

            // capture output via popen/_popen, ensure stderr is redirected to stdout
            std::string popenCmd;
#ifdef _WIN32
            // Use cmd /C to ensure redirection works on Windows
            popenCmd = std::string("cmd /C \"") + cmd + " 2>&1\"";
            FILE *pipe = _popen(popenCmd.c_str(), "r");
#else
            popenCmd = cmd + " 2>&1";
            FILE *pipe = popen(popenCmd.c_str(), "r");
#endif
            if (!pipe) {
                compileOutputLines.push_back("Failed to start compiler process");
                compileOutputVisible = true;
                updateLayout();
                return false;
            }
            char buffer[512];
            std::string out;
            while (fgets(buffer, sizeof(buffer), pipe)) {
                out += buffer;
            }
#ifdef _WIN32
            int rc = _pclose(pipe);
#else
            int rc = pclose(pipe);
#endif
            // split into lines
            std::istringstream iss(out);
            std::string l;
            while (std::getline(iss, l)) compileOutputLines.push_back(l);
            if (compileOutputLines.empty()) compileOutputLines.push_back("(no output)");
            compileOutputVisible = true;
            updateLayout();
            return rc==0;
        } catch(...) {
            compileOutputLines.push_back("Exception during compile");
            compileOutputVisible = true;
            updateLayout();
            return false;
        }
    }

    void runExecutable() {
        if (currentFile.empty()) return;
        try {
            std::filesystem::path p(currentFile);
            auto exe = p.replace_extension(".exe").string();
#ifdef _WIN32
            // Create a temporary .bat that runs the exe and then pauses
            try {
                wchar_t strSelf[MAX_PATH];
                memset(strSelf, 0, sizeof(strSelf));
                GetModuleFileNameW(NULL, strSelf, sizeof(strSelf));
                std::wstring WcurrentDir = strSelf;
                for(auto it=WcurrentDir.rbegin(); it!=WcurrentDir.rend(); ++it) {
                    if (*it == L'\\' || *it == L'/') {
                        WcurrentDir.erase(it.base()-1, WcurrentDir.end());
                        break;
                    }
                }
                WcurrentDir += L"\\";
                std::string currentDir = utf8_encode_string(WcurrentDir);
                std::string bat = currentDir + "exerun.bat";
                std::ofstream ofs(bat, std::ios::binary);
                if (ofs) {
                    ofs << "chcp 65001 > nul\r\n"; // set codepage to UTF-8
                    ofs << "@echo off\r\n";
                    ofs << "title "<< exe << "\r\n";
                    ofs << '"'<<currentDir<<"time.exe\" > nul\r\n";
                    ofs << "set t1=%errorlevel%\r\n";
                    ofs << '"' << exe << '"' << "\r\n";
                    ofs << "set exitcode=%errorlevel%\r\n";
                    ofs << '"'<<currentDir<<"time.exe\" > nul\r\n";
                    ofs << "set t2=%errorlevel%\r\n";
                    ofs << "echo ----------------------------\r\n";
                    ofs << "set /a t3=%t2%-%t1%\r\n";
                    ofs << "echo Program exited with code %exitcode%, Execution time: %t3%ms.\r\n";
                    ofs << "pause\r\n";
                    ofs.close();
                    ShellExecuteA(NULL, "open", bat.c_str(), NULL, NULL, SW_SHOW);
                } else {
                    // fallback: try to run directly without pause
                    ShellExecuteA(NULL, "open", exe.c_str(), NULL, NULL, SW_SHOW);
                }
            } catch(...) {
                // ensure no exception escapes
            }
#else
            // POSIX: run and wait for a keypress (best-effort)
            try {
                std::string cmd = std::string("\"./") + exe + "\"; echo 'Press any key to continue...'; read -n 1 -s";
                std::system(cmd.c_str());
            } catch(...) {}
#endif
        } catch(...) {}
    }

    bool hasSelection() const {
        return selection.active && !(selection.start.row==selection.end.row && selection.start.col==selection.end.col);
    }

    // 返回规范化的 selection start/end（s <= e）
    std::pair<Cursor,Cursor> normalizedSelection() const {
        Cursor s = selection.start;
        Cursor e = selection.end;
        if (s.row > e.row || (s.row==e.row && s.col>e.col)) std::swap(s,e);
        return {s,e};
    }

    // 获取选中文本（含多行，使用 '\n' 分隔）
    sf::String getSelectedText() const {
        if (!hasSelection()) return "";
        auto [s,e] = normalizedSelection();
        if (s.row==e.row) return lines[s.row].substring(s.col, e.col - s.col);
        sf::String out = lines[s.row].substring(s.col);
        for (std::size_t r = s.row+1; r < e.row; ++r) { out += '\n'; out += lines[r]; }
        out += '\n';
        out += lines[e.row].substring(0, e.col);
        return out;
    }

    // 删除当前选区，并将光标移到选区起点
    void deleteSelection() {
        pushUndo();
        if (!hasSelection()) return;
        auto [s,e] = normalizedSelection();
        if (s.row==e.row) {
            lines[s.row].erase(s.col, e.col - s.col);
            cursor = s;
        } else {
            sf::String head = lines[s.row].substring(0, s.col);
            sf::String tail = lines[e.row].substring(e.col);
            lines[s.row] = head + tail;
            // erase middle lines
            lines.erase(lines.begin()+s.row+1, lines.begin()+e.row+1);
            cursor = s;
        }
        selection.active = false;
        ensureCursorVisible();
        markModified();
        resetCursorBlink();
    }

    // 用文本替换选区（若无选区则在 cursor 处插入）
    void replaceSelectionWith(const sf::String &s) {
        if (hasSelection()) deleteSelection();
        for (auto ch: s) {
            if (ch=='\n') newline();
            else insertChar(ch);
        }
    }

    // 计算某行的像素宽度
    float getLinePixelWidth(std::size_t idx) const {
        float spaceAdvance = getGlyphAdvance(L' ', text.getCharacterSize());
        float w = 0.f;
        for (std::size_t i = 0; i < lines[idx].getSize(); ++i) {
            sf::Uint32 ch = lines[idx][i];
            if (ch == L'\t') w += spaceAdvance * tabDisplayWidth;
            else w += getGlyphAdvance(ch, text.getCharacterSize());
        }
        return w;
    }

    // 计算最长行的像素宽度
    float getMaxLineWidth() const {
        float maxw = 0.f;
        float spaceAdvance = getGlyphAdvance(L' ', text.getCharacterSize());
        for (const auto& l: lines) {
            float w = 0.f;
            for (std::size_t i = 0; i < l.getSize(); ++i) {
                sf::Uint32 ch = l[i];
                if (ch == L'\t') w += spaceAdvance * tabDisplayWidth;
                else w += getGlyphAdvance(ch, text.getCharacterSize());
            }
            maxw = std::max(maxw, w);
        }
        return maxw;
    }

    // 简单 LCS-based diff：标记左/右两侧中与另一侧不同的行
    void computeDiff(const std::vector<sf::String>& A, const std::vector<sf::String>& B,
                     std::vector<bool>& outA, std::vector<bool>& outB) {
        size_t m = A.size(), n = B.size();
        outA.assign(m, true);
        outB.assign(n, true);
        if (m == 0 || n == 0) return;
        // dp table (m+1)*(n+1)
        std::vector<std::vector<int>> dp(m+1, std::vector<int>(n+1, 0));
        for (int i = (int)m-1; i >= 0; --i) {
            for (int j = (int)n-1; j >= 0; --j) {
                if (A[i] == B[j]) dp[i][j] = 1 + dp[i+1][j+1];
                else dp[i][j] = std::max(dp[i+1][j], dp[i][j+1]);
            }
        }
        // backtrack to find LCS positions
        size_t i = 0, j = 0;
        while (i < m && j < n) {
            if (A[i] == B[j]) {
                outA[i] = false; // in LCS -> not different
                outB[j] = false;
                ++i; ++j;
            } else if (dp[i+1][j] >= dp[i][j+1]) ++i; else ++j;
        }
        // lines not in LCS remain marked true (different)
    }

    // 水平滚动，delta 为像素数（正向向右滚动）
    void scrollHoriz(int delta) {
        float contentW = box.getSize().x - lineNumberWidth - padding;
        float maxW = getMaxLineWidth();
        if (maxW <= contentW) { hScroll = 0.f; return; }
        float allowedMax = std::max(0.f, maxW - contentW + hScrollMargin);
        hScroll = std::clamp(hScroll + (float)delta, 0.f, allowedMax);
    }
};
