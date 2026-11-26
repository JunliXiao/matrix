// ============================================================
//  matrix.cpp — 終端機動畫（目前只有 Matrix 文字雨一個場景）
//
//  架構分成三塊，彼此只透過下面這幾個小型別溝通：
//
//    輸出層  ConsoleSession / Renderer / FrameClock
//            只認得「格子網格」(Cell)，完全不知道畫面上是什麼
//    面板層  InfoPanel
//            中央資訊看板，由外殼在場景畫完之後疊上去
//    模擬層  Scene（目前只有 MatrixScene）
//            決定每一格該顯示什麼，並處理自己專屬的按鍵
//
//  全域按鍵（Enter / A / S / Q）由外殼處理，其餘一律轉交給目前的場景。
//  編譯時需加 -lwinmm（timeBeginPeriod / timeEndPeriod）。
// ============================================================

#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <conio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// ============================================================
//  可調參數（想改手感都在這裡）
// ============================================================

// ---- 全域 ----
constexpr int   TARGET_FPS = 15;                    // 目標畫面更新率
constexpr float MAX_DT = 0.1f;                      // 單幀時間上限（秒），避免視窗被卡住後雨滴瞬間飛出去
constexpr ULONGLONG SYSINFO_INTERVAL_MS = 1000;     // CPU / RAM 更新間隔

// ---- Matrix 場景 ----
// Z 鍵：基礎落下速度（列／秒），依序為 慢 / 中 / 快
constexpr float SPEED_LEVELS[3] = {8.0f, 14.0f, 24.0f};
constexpr int   DEFAULT_SPEED_LEVEL = 1;            // 預設用「中」

// X 鍵：雨滴離開畫面後，到下一滴出現前最多空幾列，依序為 稀疏 / 普通 / 密集
// （數字越大越稀疏；0 表示一離開就立刻從頂端再下一滴）
constexpr int   GAP_LEVELS[3] = {120, 30, 0};
constexpr int   DEFAULT_DENSITY_LEVEL = 1;          // 預設用「普通」

// 每一欄的速度會乘上一個隨機倍率，讓雨滴不要整齊劃一
constexpr float SPEED_FACTOR_MIN = 0.6f;
constexpr float SPEED_FACTOR_MAX = 1.4f;

constexpr int   TAIL_MIN = 6;                       // 尾巴長度範圍（列）
constexpr int   TAIL_MAX = 17;
constexpr int   BODY_RATIO_PERCENT = 60;            // 尾巴前段佔幾 % 用較亮的顏色

// 尾巴上的字元平常停留不動，每幀每格有千分之幾的機率換成別的字（越大越閃）
constexpr int   FLICKER_PER_MILLE = 30;
// ============================================================


// ============================================================
//  共用型別：輸出層與場景之間的「合約」
// ============================================================

// 調色盤編號：Cell.color 存的是「目前場景調色盤」的索引，輸出時才換成實際的跳脫序列。
// 0 保留給空白格；1、2 是中央面板用的固定色位（每個場景都要填）；3 起由場景自行使用。
constexpr uint8_t PAL_NONE         = 0;
constexpr uint8_t PAL_PANEL_BORDER = 1;
constexpr uint8_t PAL_PANEL_TEXT   = 2;
constexpr uint8_t PAL_SCENE_0      = 3;
constexpr int     PAL_SIZE         = 16;            // 調色盤陣列長度

// 畫面上的一個格子：只記「顯示什麼、用哪種顏色」。
//   glyph: Unicode 碼點（僅限 BMP，且不可是代理對 D800–DFFF）；0 = 空白
//   color: 調色盤索引（見上）
struct Cell {
    uint16_t glyph = 0;
    uint8_t  color = PAL_NONE;
    bool operator==(const Cell& o) const { return glyph == o.glyph && color == o.color; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

// 這個值不會出現在真正的畫面上（U+FFFF 是非字元）；把「上一幀」填滿它，下一幀就會整幀重畫
constexpr Cell INVALID_CELL = {0xFFFF, 0xFF};

// 半開區間 [x0,x1) × [y0,y1)；預設是空矩形
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};


// ============================================================
//  輸出層
// ============================================================

// 把一個 BMP 碼點編成 UTF-8 附加到字串尾端
static void appendUtf8(std::string& s, uint16_t cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static void appendInt(std::string& s, int v) {
    char buf[12];
    int n = 0;
    if (v <= 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = static_cast<char>('0' + v % 10); v /= 10; }
    while (n > 0) s += buf[--n];
}

// 游標移到第 row 列、第 col 欄（都從 1 開始）
static void appendCursorPos(std::string& s, int row, int col) {
    s += "\x1B[";
    appendInt(s, row);
    s += ';';
    appendInt(s, col);
    s += 'H';
}

// 直接用 WriteFile 輸出，繞過 CRT 的緩衝；單次上限 32KB，避免整幀重畫時單次寫入過大
static void writeAll(HANDLE h, const char* p, size_t n) {
    while (n > 0) {
        DWORD toWrite = static_cast<DWORD>(n > 32768 ? 32768 : n);
        DWORD written = 0;
        if (!WriteFile(h, p, toWrite, &written, nullptr) || written == 0) break;
        p += written;
        n -= written;
    }
}

static void writeStr(HANDLE h, const char* s) { writeAll(h, s, std::strlen(s)); }

// 主控台的啟動與還原：UTF-8、ANSI 跳脫序列、關閉快速編輯、1ms 計時器精度、隱藏游標
class ConsoleSession {
public:
    ConsoleSession() {
        SetConsoleOutputCP(CP_UTF8);
        hOut_ = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD outMode = 0;
        GetConsoleMode(hOut_, &outMode);
        SetConsoleMode(hOut_, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        // 關閉「快速編輯模式」：否則滑鼠點一下會進入文字選取，輸出被擋住、動畫就暫停
        hIn_ = GetStdHandle(STD_INPUT_HANDLE);
        GetConsoleMode(hIn_, &origInMode_);
        SetConsoleMode(hIn_, (origInMode_ & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);

        // 把系統計時器精度拉到 1ms：預設約 15.6ms，Sleep 會被湊整，影格間隔就會忽長忽短
        timeBeginPeriod(1);

        // 隱藏游標並清空螢幕
        writeStr(hOut_, "\x1B[?25l\x1B[2J");
    }

    // 結束時恢復游標、顏色與主控台輸入模式
    ~ConsoleSession() {
        writeStr(hOut_, "\x1B[0m\x1B[?25h\x1B[2J\x1B[H");
        SetConsoleMode(hIn_, origInMode_);
        timeEndPeriod(1);
    }

    ConsoleSession(const ConsoleSession&) = delete;
    ConsoleSession& operator=(const ConsoleSession&) = delete;

    HANDLE out() const { return hOut_; }

    // 目前可見視窗的大小（不是整個緩衝區）；查詢失敗時回傳 false，呼叫端沿用舊值
    bool querySize(int& w, int& h) const {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (!GetConsoleScreenBufferInfo(hOut_, &csbi)) return false;
        w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return true;
    }

private:
    HANDLE hOut_ = nullptr;
    HANDLE hIn_ = nullptr;
    DWORD  origInMode_ = 0;
};

// 格子網格 + 差異比對輸出。場景只負責把這一幀寫進 beginFrame() 給的陣列，
// 其餘（和上一幀比對、只送有變的格子、組跳脫序列）都在這裡。
class Renderer {
public:
    // 視窗大小改變：重新配置，並讓下一幀整幀重畫
    void resize(int w, int h) {
        w_ = w;
        h_ = h;
        const size_t cellCount = static_cast<size_t>(w) * h;
        cur_.assign(cellCount, Cell{});
        prev_.assign(cellCount, INVALID_CELL);
        // 預先配置足夠的記憶體空間（以整幀重畫的最壞情況估算），執行期間幾乎零記憶體重新分配
        frame_.reserve(cellCount * 16);
    }

    // 格子內容沒變、但「索引對應到的顏色或字」變了（切換場景、換主題）時呼叫：下一幀整幀重畫
    void invalidate() { std::fill(prev_.begin(), prev_.end(), INVALID_CELL); }

    // 開始新的一幀：回傳已清成空白的格子陣列（w×h，列優先）
    Cell* beginFrame() {
        std::fill(cur_.begin(), cur_.end(), Cell{});
        return cur_.data();
    }

    // 和上一幀比對，只輸出有變的格子
    void present(HANDLE hOut, const char* const* palette) {
        // 開始同步更新（支援的終端機會等整幀收齊才一次顯示，減少撕裂；不支援的會直接忽略）
        frame_ = "\x1B[?2026h";

        const char* curColor = nullptr; // 目前終端機正在使用的顏色（nullptr = 預設）
        int lastY = -1, lastX = -2;     // 上一個輸出的格子位置，用來判斷游標是否已經在正確位置
        bool anyChange = false;

        for (int y = 0; y < h_; ++y) {
            for (int x = 0; x < w_; ++x) {
                const size_t idx = static_cast<size_t>(y) * w_ + x;
                const Cell& cell = cur_[idx];
                if (cell == prev_[idx]) continue;

                anyChange = true;
                // 緊接在上一個輸出的格子後面時，游標自己會往右一格，不必再定位
                if (!(y == lastY && x == lastX + 1))
                    appendCursorPos(frame_, y + 1, x + 1);

                if (cell.glyph == 0) {
                    frame_ += ' '; // 空白看不出前景色，不需要設色
                } else {
                    const char* color = palette[cell.color];
                    if (color && color != curColor) {
                        curColor = color;
                        frame_ += curColor;
                    }
                    appendUtf8(frame_, cell.glyph);
                }
                lastY = y;
                lastX = x;
            }
        }

        if (curColor) frame_ += "\x1B[0m"; // 整幀結束才重置一次
        frame_ += "\x1B[?2026l";           // 結束同步更新

        // 這一幀就是螢幕上現在的樣子，下一幀拿它來比對（交換比複製快）
        prev_.swap(cur_);

        if (anyChange) writeAll(hOut, frame_.data(), frame_.size());
    }

private:
    int w_ = 0, h_ = 0;
    std::vector<Cell> cur_;   // 這一幀每格該顯示的內容
    std::vector<Cell> prev_;  // 上一幀實際顯示在螢幕上的內容
    std::string frame_;
};

// 固定節奏的影格計時：記錄「下一幀應該開始的時間」，而不是每幀固定 Sleep
class FrameClock {
public:
    explicit FrameClock(int fps) {
        QueryPerformanceFrequency(&freq_);
        frameTicks_ = freq_.QuadPart / fps;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        nextFrame_ = now.QuadPart + frameTicks_;
        prev_ = now.QuadPart;
    }

    // 距離上一次呼叫經過的時間（秒），並套用 MAX_DT 上限。
    // 雨滴依「實際經過的時間」前進，不論終端機顯示得快或慢，速度都一樣
    float consumeDt() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - prev_) / static_cast<float>(freq_.QuadPart);
        prev_ = now.QuadPart;
        return dt > MAX_DT ? MAX_DT : dt;
    }

    // 睡到下一幀的預定時間；這一幀花多久就少睡多久，節奏才會穩
    void waitNextFrame() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const LONGLONG remain = nextFrame_ - now.QuadPart;
        if (remain > 0) {
            Sleep(static_cast<DWORD>(remain * 1000 / freq_.QuadPart));
        } else {
            nextFrame_ = now.QuadPart; // 已經落後（例如視窗剛縮放），重新對齊，不連續補幀
        }
        nextFrame_ += frameTicks_;
    }

private:
    LARGE_INTEGER freq_{};
    LONGLONG frameTicks_ = 0;
    LONGLONG nextFrame_ = 0;
    LONGLONG prev_ = 0;
};


// ============================================================
//  面板層：中央資訊看板
// ============================================================

static ULONGLONG fileTimeToU64(const FILETIME& ft) {
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// CPU 使用率要看「兩次取樣之間」的差值，所以需要記住上一次的數值
struct CpuSampler {
    ULONGLONG prevIdle = 0;
    ULONGLONG prevTotal = 0;
    int usage = 0; // 0~100

    void update() {
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) return;
        ULONGLONG i = fileTimeToU64(idle);
        ULONGLONG total = fileTimeToU64(kernel) + fileTimeToU64(user); // kernel 已包含 idle
        if (prevTotal != 0) {
            ULONGLONG dTotal = total - prevTotal;
            ULONGLONG dIdle = i - prevIdle;
            if (dTotal > 0) usage = static_cast<int>((dTotal - dIdle) * 100 / dTotal);
        }
        prevIdle = i;
        prevTotal = total;
    }
};

static int getRamUsage() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0;
    return static_cast<int>(ms.dwMemoryLoad); // 系統直接給 0~100
}

// 10 格進度條。空格子用 '.'，避免被上色邏輯誤判成邊框字元 '-'
static std::string makeBar(int pct) {
    pct = std::max(0, std::min(100, pct));
    int filled = pct / 10;
    return std::string(filled, '#') + std::string(10 - filled, '.');
}

// 中央看板。每幀的流程：refresh() 取得「保留區」→ 場景畫它自己的 → draw() 把看板疊上去。
// 看板顏色不在這裡決定：邊框用 PAL_PANEL_BORDER、文字用 PAL_PANEL_TEXT，由目前場景的調色盤給值。
class InfoPanel {
public:
    InfoPanel() {
        cpu_.update(); // 先建立基準值
        ramPct_ = getRamUsage();
        lastSample_ = GetTickCount64();
    }

    void toggle() { visible_ = !visible_; }

    // 每幀呼叫。回傳看板佔用的保留區（含四周各 1 格留白）；看板關閉時回傳空矩形。
    // CPU / RAM 即使看板關閉也照常每秒取樣（成本可忽略），這樣重新開啟時讀數是新的，
    // 而不是「關閉期間的平均值」。
    Rect refresh(int w, int h) {
        const ULONGLONG nowTick = GetTickCount64();
        if (nowTick - lastSample_ >= SYSINFO_INTERVAL_MS) {
            cpu_.update();
            ramPct_ = getRamUsage();
            lastSample_ = nowTick;
        }

        if (!visible_) {
            reserved_ = Rect{};
            return reserved_;
        }

        buildLines();
        boxH_ = static_cast<int>(lines_.size());
        boxW_ = static_cast<int>(lines_[0].size());
        startY_ = (h - boxH_) / 2;
        startX_ = (w - boxW_) / 2;
        reserved_ = Rect{startX_ - 1, startY_ - 1, startX_ + boxW_ + 1, startY_ + boxH_ + 1};
        return reserved_;
    }

    // 把看板寫進格子陣列：先把保留區清成空白（不論場景畫了什麼），再畫文字
    void draw(Cell* grid, int w, int h) const {
        if (!visible_) return;

        const int yBeg = std::max(0, reserved_.y0), yEnd = std::min(h, reserved_.y1);
        const int xBeg = std::max(0, reserved_.x0), xEnd = std::min(w, reserved_.x1);
        for (int y = yBeg; y < yEnd; ++y)
            for (int x = xBeg; x < xEnd; ++x)
                grid[static_cast<size_t>(y) * w + x] = Cell{};

        for (int r = 0; r < boxH_; ++r) {
            const int y = startY_ + r;
            if (y < 0 || y >= h) continue;
            for (int c = 0; c < boxW_; ++c) {
                const int x = startX_ + c;
                if (x < 0 || x >= w) continue;
                const char ch = lines_[r][c];
                if (ch == ' ') continue; // 保留區已經清成空白
                const bool isBorder = (ch == '+' || ch == '-' || ch == '|');
                grid[static_cast<size_t>(y) * w + x] =
                    Cell{static_cast<uint16_t>(static_cast<unsigned char>(ch)),
                         isBorder ? PAL_PANEL_BORDER : PAL_PANEL_TEXT};
            }
        }
    }

private:
    void buildLines() {
        // 取得目前系統時間
        std::time_t t = std::time(nullptr);
        std::tm nowTm{};
        localtime_s(&nowTm, &t); // 執行緒安全版本，不用處理回傳指標

        char dateTimeBuf[32];
        std::snprintf(dateTimeBuf, sizeof(dateTimeBuf), "|  %04d-%02d-%02d  %02d:%02d:%02d  |",
                      nowTm.tm_year + 1900, nowTm.tm_mon + 1, nowTm.tm_mday,
                      nowTm.tm_hour, nowTm.tm_min, nowTm.tm_sec);

        char cpuBuf[32], ramBuf[32];
        std::snprintf(cpuBuf, sizeof(cpuBuf), "|  CPU [%s] %3d%% |", makeBar(cpu_.usage).c_str(), cpu_.usage);
        std::snprintf(ramBuf, sizeof(ramBuf), "|  RAM [%s] %3d%% |", makeBar(ramPct_).c_str(), ramPct_);

        lines_.assign({
            "+------------------------+",
            "|   [ SYSTEM  ONLINE ]   |",
            dateTimeBuf,
            "|------------------------|",
            cpuBuf,
            ramBuf,
            "+------------------------+"
        });

        // 寬度是「每行的字元數」，不是行數。取最長那一行，並把較短的行補空白，
        // 避免之後 lines_[r][c] 越界。
        size_t maxW = 0;
        for (const auto& line : lines_)
            maxW = std::max(maxW, line.size());
        for (auto& line : lines_)
            if (line.size() < maxW) line.append(maxW - line.size(), ' ');
    }

    bool visible_ = true;     // 程式啟動時固定顯示
    CpuSampler cpu_;
    int ramPct_ = 0;
    ULONGLONG lastSample_ = 0;
    std::vector<std::string> lines_;
    int boxW_ = 0, boxH_ = 0, startX_ = 0, startY_ = 0;
    Rect reserved_;
};


// ============================================================
//  模擬層：Scene 介面
// ============================================================

class Scene {
public:
    virtual ~Scene() = default;

    // 視窗大小改變，或這個場景剛被切換進來時呼叫。在這裡重新配置、重新散佈內部狀態。
    virtual void resize(int w, int h) = 0;

    // 依實際經過的時間 dt（秒）推進模擬。
    virtual void update(float dt) = 0;

    // 把這一幀的畫面寫進 grid（w×h，列優先；呼叫前已清成空白，w、h 永遠等於最後一次 resize）。
    // reserved 是中央看板的保留區：場景不必在那裡畫東西，外殼畫完之後會再清一次並疊上看板；
    // 有「逐格狀態」的場景可以順便在這裡重設該區的狀態。看板關閉時 reserved 是空矩形。
    virtual void draw(Cell* grid, int w, int h, const Rect& reserved) = 0;

    // 場景專屬按鍵（外殼已處理掉 Enter / A / S / Q）。字母鍵一律是大寫，方向鍵是 K_UP 等代碼。
    // 回傳 true 表示「索引對應到的顏色或字變了」，外殼會讓下一幀整幀重畫。
    virtual bool onKey(int key) = 0;

    // 目前的調色盤：PAL_SIZE 個元素，索引 = Cell.color，未使用的填 nullptr。
    virtual const char* const* palette() const = 0;
};


// ============================================================
//  Matrix 文字雨場景
// ============================================================

class MatrixScene : public Scene {
public:
    MatrixScene() {
        // 半形片假名 U+FF71–FF9D（共 45 個）與英數字元集。
        // 每個元素在終端機都精準佔 1 格寬度，字元直接以碼點表示，不依賴原始碼檔案的編碼。
        for (uint16_t cp = 0xFF71; cp <= 0xFF9D; ++cp) chars_.push_back(cp);
        for (char c = '0'; c <= '9'; ++c) chars_.push_back(static_cast<uint16_t>(c));
        for (char c = 'A'; c <= 'Z'; ++c) chars_.push_back(static_cast<uint16_t>(c));
        for (const char* p = "@#$%&*"; *p; ++p) chars_.push_back(static_cast<uint16_t>(*p));
        rebuildPalette();
    }

    void resize(int w, int h) override {
        width_ = w;
        height_ = h;
        drops_.resize(w);
        headRows_.resize(w);
        for (int x = 0; x < w; ++x)
            initDrop(drops_[x], h); // 開場散佈在畫面上方 0 ~ height 列之外
        rainGlyph_.assign(static_cast<size_t>(w) * h, 0);
    }

    // 更新雨滴座標：依「實際經過的時間」前進，而不是每幀固定走一格
    void update(float dt) override {
        const float baseStep = SPEED_LEVELS[speedLevel_] * dt;
        for (auto& d : drops_) {
            d.pos += baseStep * d.speedFactor;
            if (d.pos - d.tailLen > height_) {
                // 整條尾巴都離開畫面了：依目前密度決定下一滴多久之後出現
                initDrop(d, GAP_LEVELS[densityLevel_]);
            }
        }
    }

    void draw(Cell* grid, int w, int h, const Rect& reserved) override {
        // 每欄頭部的整數列只需每幀算一次，不用在每個格子裡重算
        for (int x = 0; x < w; ++x)
            headRows_[x] = static_cast<int>(std::floor(drops_[x].pos));

        for (int y = 0; y < h; ++y) {
            const bool rowReserved = (y >= reserved.y0 && y < reserved.y1);
            for (int x = 0; x < w; ++x) {
                const size_t idx = static_cast<size_t>(y) * w + x;
                uint16_t& glyph = rainGlyph_[idx];

                if (rowReserved && x >= reserved.x0 && x < reserved.x1) {
                    glyph = 0; // 看板底下不畫雨，也不留字元
                    continue;  // grid 已經是空白
                }

                const int headY = headRows_[x];
                const int tailLen = drops_[x].tailLen;

                if (y <= headY && y >= headY - tailLen) {
                    // 雨滴經過這格：第一次被蓋到時才抽字，之後字元留在原地；
                    // 只有尾巴上的格子會偶爾換字（頭部不換），形成零星閃動
                    if (glyph == 0 || (y != headY && std::rand() % 1000 < FLICKER_PER_MILLE))
                        glyph = chars_[std::rand() % chars_.size()];

                    uint8_t color;
                    if (y == headY)
                        color = C_HEAD;
                    else // 整數比較，避開浮點運算
                        color = ((headY - y) * 100 < tailLen * BODY_RATIO_PERCENT) ? C_BODY : C_TAIL;
                    grid[idx] = Cell{glyph, color};
                } else {
                    glyph = 0; // 尾巴離開這格，字元消失
                }
            }
        }
    }

    bool onKey(int key) override {
        switch (key) {
        case ' ': // 空白鍵切換配色
            themeIndex_ = (themeIndex_ + 1) % 3;
            rebuildPalette();
            return true; // 格子內容沒變、只有顏色碼變了，所以要整幀重畫
        case 'Z': // 落下速度循環
            speedLevel_ = (speedLevel_ + 1) % 3;
            return false;
        case 'X': // 密度循環
            densityLevel_ = (densityLevel_ + 1) % 3;
            // 還在畫面上方排隊的雨滴立刻依新密度重抽位置，已在畫面上的讓它自然落完
            for (auto& d : drops_) {
                if (d.pos < 0.0f)
                    d.pos = -static_cast<float>(std::rand() % std::max(1, GAP_LEVELS[densityLevel_]));
            }
            return false;
        default:
            return false;
        }
    }

    const char* const* palette() const override { return pal_; }

private:
    // 這個場景自己用的色位
    static constexpr uint8_t C_HEAD = PAL_SCENE_0;
    static constexpr uint8_t C_BODY = PAL_SCENE_0 + 1;
    static constexpr uint8_t C_TAIL = PAL_SCENE_0 + 2;

    // 三種顏色主題（駭客綠 / 賽博龐克粉藍 / 警報紅）
    struct Theme {
        const char* head;
        const char* body;
        const char* tail;
        const char* box;
    };
    static constexpr Theme THEMES[3] = {
        {"\x1B[1;97m", "\x1B[38;5;46m",  "\x1B[38;5;22m",  "\x1B[1;38;5;46m"},
        {"\x1B[1;97m", "\x1B[38;5;51m",  "\x1B[38;5;201m", "\x1B[1;38;5;213m"},
        {"\x1B[1;93m", "\x1B[38;5;196m", "\x1B[38;5;88m",  "\x1B[1;38;5;196m"}
    };
    static constexpr const char* COLOR_WHITE = "\x1B[1;97m"; // 看板文字用的白色

    // 每一欄一滴雨：位置用浮點數，才能依「經過時間」平滑前進
    struct Drop {
        float pos = 0.0f;         // 頭部所在的列，可為小數；負數表示還在畫面上方排隊
        float speedFactor = 1.0f; // 這一欄的速度倍率
        int   tailLen = TAIL_MIN;
    };

    // 重新生成一滴雨：從畫面上方 0 ~ maxAbove 列之外開始，速度倍率與尾巴長度重新隨機
    static void initDrop(Drop& d, int maxAbove) {
        d.pos = -static_cast<float>(std::rand() % std::max(1, maxAbove));
        float r = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        d.speedFactor = SPEED_FACTOR_MIN + (SPEED_FACTOR_MAX - SPEED_FACTOR_MIN) * r;
        d.tailLen = TAIL_MIN + std::rand() % (TAIL_MAX - TAIL_MIN + 1);
    }

    void rebuildPalette() {
        const Theme& t = THEMES[themeIndex_];
        pal_[PAL_PANEL_BORDER] = t.box;
        pal_[PAL_PANEL_TEXT]   = COLOR_WHITE;
        pal_[C_HEAD] = t.head;
        pal_[C_BODY] = t.body;
        pal_[C_TAIL] = t.tail;
    }

    int width_ = 0, height_ = 0;
    std::vector<uint16_t> chars_;      // 雨滴字元集（碼點）
    std::vector<Drop> drops_;
    std::vector<int> headRows_;        // 每欄雨滴頭部所在的整數列（每幀只算一次）
    std::vector<uint16_t> rainGlyph_;  // 每格目前停留的雨滴字元（0 = 沒有），字元會跟著格子留在原地
    int themeIndex_ = 0;               // 空白鍵切換
    int speedLevel_ = DEFAULT_SPEED_LEVEL;     // Z 鍵切換
    int densityLevel_ = DEFAULT_DENSITY_LEVEL; // X 鍵切換
    const char* pal_[PAL_SIZE] = {};
};


// ============================================================
//  按鍵
// ============================================================

// 方向鍵的代碼：刻意放在字元範圍之外，才不會和一般按鍵撞在一起
enum Key : int { K_UP = 0x1000, K_DOWN, K_LEFT, K_RIGHT };

// 讀一個按鍵（呼叫前請先確認 _kbhit()）。
//  - 方向鍵等擴充鍵在 _getch 會分成兩次回傳：先 0 或 0xE0，再來才是實際鍵碼。
//    這裡一次讀完，方向鍵轉成 K_*，其他擴充鍵回傳 -1（呼叫端應忽略）；
//    否則第二個碼會被誤當成一般按鍵（例如 Delete 的 0x53 就是 'S'）。
//  - 字母一律轉成大寫，場景與外殼只需要判斷大寫。
static int readKey() {
    const int ch = _getch();
    if (ch == 0 || ch == 0xE0) {
        switch (_getch()) {
        case 0x48: return K_UP;
        case 0x50: return K_DOWN;
        case 0x4B: return K_LEFT;
        case 0x4D: return K_RIGHT;
        default:   return -1;
        }
    }
    return (ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch;
}


// ============================================================
//  外殼：主迴圈
// ============================================================

int main() {
    ConsoleSession console;
    const HANDLE hOut = console.out();

    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<MatrixScene>());
    size_t sceneIdx = 0;

    Renderer renderer;
    InfoPanel panel;
    FrameClock frameClock(TARGET_FPS);

    int width = 0, height = 0;

    // 切換場景：新場景依目前視窗大小重新配置；索引對應的字與顏色都換了，所以整幀重畫
    auto switchTo = [&](size_t next) {
        if (next == sceneIdx) return;
        sceneIdx = next;
        scenes[sceneIdx]->resize(width, height);
        renderer.invalidate();
    };

    while (true) {
        // 1. 偵測鍵盤按鍵（無需按 Enter，非阻塞式偵測）
        if (_kbhit()) {
            const int key = readKey();
            if (key == '\r') {                          // Enter：看板開關
                panel.toggle();
            } else if (key == 'A' || key == 'S') {      // A / S：上一個 / 下一個場景（循環）
                const size_t n = scenes.size();
                switchTo(key == 'A' ? (sceneIdx + n - 1) % n : (sceneIdx + 1) % n);
            } else if (key == 'Q' || key == 27 || key == 3) {
                break;                                  // 按 Q、ESC 或 Ctrl+C 退出
            } else if (key >= 0) {                      // 其餘交給目前的場景
                if (scenes[sceneIdx]->onKey(key)) renderer.invalidate();
            }
        }

        // 2. 動態取得目前終端機視窗大小（支援即時拉大縮小 RWD）
        int curW = width, curH = height;
        console.querySize(curW, curH);
        if (curW != width || curH != height) {
            width = curW;
            height = curH;
            writeStr(hOut, "\x1B[2J");
            renderer.resize(width, height);
            scenes[sceneIdx]->resize(width, height);
        }

        // 3. 組出這一幀：場景先畫，看板再疊上去
        Scene& scene = *scenes[sceneIdx];
        const Rect reserved = panel.refresh(width, height);
        Cell* grid = renderer.beginFrame();
        scene.draw(grid, width, height, reserved);
        panel.draw(grid, width, height);

        // 4. 和上一幀比對並輸出
        renderer.present(hOut, scene.palette());

        // 5. 推進模擬，然後睡到下一幀
        scene.update(frameClock.consumeDt());
        frameClock.waitNextFrame();
    }

    return 0;
}