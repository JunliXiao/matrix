#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <windows.h>
#include <mmsystem.h> // timeBeginPeriod / timeEndPeriod（編譯時需加 -lwinmm）
#include <conio.h>
#include <cmath>
#include <cstdint>

// ============================================================
//  可調參數（想改手感都在這裡）
// ============================================================
constexpr int   TARGET_FPS = 15;                    // 目標畫面更新率

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

constexpr float MAX_DT = 0.1f;                      // 單幀時間上限（秒），避免視窗被卡住後雨滴瞬間飛出去
constexpr ULONGLONG SYSINFO_INTERVAL_MS = 1000;     // CPU / RAM 更新間隔
// ============================================================

struct Theme {
    const char* head;
    const char* body;
    const char* tail;
    const char* box;
};

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

// 畫面上的一個格子：只記「顯示什麼、用哪種顏色」，輸出時才換成實際的跳脫序列
//   glyph: 0 = 空白；1 ~ 0xFF = 雨滴字元（chars 的索引 + 1）；0x100 | c = 看板上的 ASCII 字元 c
//   color: 下面 ColorId 的值
enum ColorId : uint8_t { C_NONE = 0, C_HEAD, C_BODY, C_TAIL, C_BOX, C_WHITE, C_COUNT };

struct Cell {
    uint16_t glyph = 0;
    uint8_t  color = C_NONE;
    bool operator==(const Cell& o) const { return glyph == o.glyph && color == o.color; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

// 這個值不會出現在真正的畫面上；把「上一幀」填滿它，下一幀就會整幀重畫
constexpr Cell INVALID_CELL = {0xFFFF, 0xFF};

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

int main() {
    // 1. 啟用 Windows 終端機的 UTF-8 輸出與 ANSI 256色支援
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // 關閉「快速編輯模式」：否則滑鼠點一下會進入文字選取，輸出被擋住、動畫就暫停
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD origInMode = 0;
    GetConsoleMode(hIn, &origInMode);
    SetConsoleMode(hIn, (origInMode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);

    // 把系統計時器精度拉到 1ms：預設約 15.6ms，Sleep 會被湊整，影格間隔就會忽長忽短
    timeBeginPeriod(1);

    // 隱藏游標並清空螢幕
    std::cout << "\x1B[?25l\x1B[2J" << std::flush;

    // 2. 定義三種顏色主題（駭客綠 / 賽博龐克粉藍 / 警報紅）
    Theme themes[3] = {
        {"\x1B[1;97m", "\x1B[38;5;46m",  "\x1B[38;5;22m",  "\x1B[1;38;5;46m"},
        {"\x1B[1;97m", "\x1B[38;5;51m",  "\x1B[38;5;201m", "\x1B[1;38;5;213m"},
        {"\x1B[1;93m", "\x1B[38;5;196m", "\x1B[38;5;88m",  "\x1B[1;38;5;196m"}
    };
    int themeIndex = 0;
    const char* COLOR_RESET = "\x1B[0m";
    const char* COLOR_WHITE = "\x1B[1;97m"; // 看板文字用的白色

    // 半形片假名與英數字元集（UTF-8 陣列，確保每個元素精準佔 1 格寬度）
    const std::vector<std::string> chars = {
        "ｱ","ｲ","ｳ","ｴ","ｵ","ｶ","ｷ","ｸ","ｹ","ｺ","ｻ","ｼ","ｽ","ｾ","ｿ",
        "ﾀ","ﾁ","ﾂ","ﾃ","ﾄ","ﾅ","ﾆ","ﾇ","ﾈ","ﾉ","ﾊ","ﾋ","ﾌ","ﾍ","ﾎ",
        "ﾏ","ﾐ","ﾑ","ﾒ","ﾓ","ﾔ","ﾕ","ﾖ","ﾗ","ﾘ","ﾙ","ﾚ","ﾛ","ﾜ","ﾝ",
        "0","1","2","3","4","5","6","7","8","9",
        "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
        "@","#","$","%","&","*"
    };

    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    int width = 0, height = 0;
    std::vector<Drop> drops;
    std::vector<int> headRows; // 每欄雨滴頭部所在的整數列（每幀只算一次）
    std::vector<Cell> curGrid;       // 這一幀每格該顯示的內容
    std::vector<Cell> prevGrid;      // 上一幀實際顯示在螢幕上的內容（用來比對差異）
    std::vector<uint16_t> rainGlyph; // 每格目前停留的雨滴字元（0 = 沒有），字元會跟著格子留在原地
    bool needFullRedraw = true;      // 視窗縮放或切換主題時，要整幀重畫一次
    std::string frameBuffer;
    int speedLevel = DEFAULT_SPEED_LEVEL;     // Z 鍵切換
    int densityLevel = DEFAULT_DENSITY_LEVEL; // X 鍵切換

    // 系統資訊：每秒取樣一次即可，每幀都取 CPU 數值會很跳
    CpuSampler cpu;
    cpu.update(); // 先建立基準值
    int ramPct = getRamUsage();
    ULONGLONG lastSample = GetTickCount64();

    // 固定節奏的影格計時：記錄「下一幀應該開始的時間」，而不是每幀固定 Sleep(50)
    LARGE_INTEGER qpcFreq, qpcNow;
    QueryPerformanceFrequency(&qpcFreq);
    const LONGLONG frameTicks = qpcFreq.QuadPart / TARGET_FPS;
    QueryPerformanceCounter(&qpcNow);
    LONGLONG nextFrame = qpcNow.QuadPart + frameTicks;
    LONGLONG prevCounter = qpcNow.QuadPart; // 上一次更新雨滴的時間點，用來算 dt

    while (true) {
        // 3. 偵測鍵盤按鍵（無需按 Enter，非阻塞式偵測）
        if (_kbhit()) {
            int ch = _getch();
            if (ch == ' ') {
                themeIndex = (themeIndex + 1) % 3; // 空白鍵切換主題
                needFullRedraw = true; // 格子內容沒變、只有顏色碼變了，所以要強制整幀重畫
            } else if (ch == 'z' || ch == 'Z') {
                speedLevel = (speedLevel + 1) % 3; // Z 鍵切換落下速度
            } else if (ch == 'x' || ch == 'X') {
                densityLevel = (densityLevel + 1) % 3; // X 鍵切換密度
                // 還在畫面上方排隊的雨滴立刻依新密度重抽位置，已在畫面上的讓它自然落完
                for (auto& d : drops) {
                    if (d.pos < 0.0f)
                        d.pos = -static_cast<float>(std::rand() % std::max(1, GAP_LEVELS[densityLevel]));
                }
            } else if (ch == 'q' || ch == 'Q' || ch == 27 || ch == 3) {
                break; // 按 Q、ESC 或 Ctrl+C 退出
            }
        }

        // 4. 動態取得目前終端機視窗大小（支援即時拉大縮小 RWD）
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hOut, &csbi);
        int currWidth = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        int currHeight = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

        if (currWidth != width || currHeight != height) {
            width = currWidth;
            height = currHeight;
            std::cout << "\x1B[2J" << std::flush;
            drops.resize(width);
            headRows.resize(width);
            for (int x = 0; x < width; ++x)
                initDrop(drops[x], height); // 開場散佈在畫面上方 0 ~ height 列之外
            // 格子陣列依新的視窗大小重新配置，雨滴字元清空
            const size_t cellCount = static_cast<size_t>(width) * height;
            curGrid.assign(cellCount, Cell{});
            prevGrid.assign(cellCount, INVALID_CELL);
            rainGlyph.assign(cellCount, 0);
            needFullRedraw = true;
            // 預先配置足夠的記憶體空間（以整幀重畫的最壞情況估算），執行期間幾乎零記憶體重新分配
            frameBuffer.reserve(cellCount * 16);
        }

        // 5. 取得目前系統時間並組裝中央看板
        std::time_t t = std::time(nullptr);
        std::tm nowTm{};
        localtime_s(&nowTm, &t); // 執行緒安全版本，不用處理回傳指標

        char dateTimeBuf[32];
        std::snprintf(dateTimeBuf, sizeof(dateTimeBuf), "|  %04d-%02d-%02d  %02d:%02d:%02d  |",
                      nowTm.tm_year + 1900, nowTm.tm_mon + 1, nowTm.tm_mday,
                      nowTm.tm_hour, nowTm.tm_min, nowTm.tm_sec);

//             "|  [SPACE] SWITCH THEME  |",
        // 定時更新 CPU / RAM
        ULONGLONG nowTick = GetTickCount64();
        if (nowTick - lastSample >= SYSINFO_INTERVAL_MS) {
            cpu.update();
            ramPct = getRamUsage();
            lastSample = nowTick;
        }

        char cpuBuf[32], ramBuf[32];
        std::snprintf(cpuBuf, sizeof(cpuBuf), "|  CPU [%s] %3d%% |", makeBar(cpu.usage).c_str(), cpu.usage);
        std::snprintf(ramBuf, sizeof(ramBuf), "|  RAM [%s] %3d%% |", makeBar(ramPct).c_str(), ramPct);

        std::vector<std::string> clockLines = {
            "+------------------------+",
            "|   [ SYSTEM  ONLINE ]   |",
            dateTimeBuf,
            "|------------------------|",
            cpuBuf,
            ramBuf,
            "+------------------------+"
        };

        // 【修正】寬度是「每行的字元數」，不是行數。取最長那一行，
        // 並把較短的行補空白，避免之後 clockLines[y][x] 越界。
        size_t maxW = 0;
        for (const auto& line : clockLines)
            maxW = std::max(maxW, line.size());
        for (auto& line : clockLines)
            if (line.size() < maxW) line.append(maxW - line.size(), ' ');

        const int boxHeight = static_cast<int>(clockLines.size());
        const int boxWidth  = static_cast<int>(maxW);
        const int startY = (height - boxHeight) / 2;
        const int startX = (width - boxWidth) / 2;

        // 顏色編號 → 目前主題的實際跳脫序列（切換主題只需要換這張表）
        const Theme& theme = themes[themeIndex];
        const char* palette[C_COUNT] = {nullptr, theme.head, theme.body, theme.tail, theme.box, COLOR_WHITE};

        // 每欄頭部的整數列只需每幀算一次，不用在每個格子裡重算
        for (int x = 0; x < width; ++x)
            headRows[x] = static_cast<int>(std::floor(drops[x].pos));

        // 6. 先決定「每一格該顯示什麼」，寫進 curGrid（這一步還不輸出任何東西）
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t idx = static_cast<size_t>(y) * width + x;
                bool inBoxY = (y >= startY && y < startY + boxHeight);
                bool inBoxX = (x >= startX && x < startX + boxWidth);
                bool inPadY = (y >= startY - 1 && y <= startY + boxHeight);
                bool inPadX = (x >= startX - 1 && x <= startX + boxWidth);

                if (inBoxY && inBoxX) {
                    rainGlyph[idx] = 0;
                    char c = clockLines[y - startY][x - startX];
                    if (c == ' ') {
                        curGrid[idx] = Cell{};
                    } else {
                        bool isBorder = (c == '+' || c == '-' || c == '|');
                        curGrid[idx] = Cell{static_cast<uint16_t>(0x100 | static_cast<unsigned char>(c)),
                                            static_cast<uint8_t>(isBorder ? C_BOX : C_WHITE)};
                    }
                } else if (inPadY && inPadX) {
                    rainGlyph[idx] = 0;
                    curGrid[idx] = Cell{};
                } else {
                    int headY = headRows[x];
                    int tailLen = drops[x].tailLen;
                    uint16_t& glyph = rainGlyph[idx];

                    if (y <= headY && y >= headY - tailLen) {
                        // 雨滴經過這格：第一次被蓋到時才抽字，之後字元留在原地；
                        // 只有尾巴上的格子會偶爾換字（頭部不換），形成零星閃動
                        if (glyph == 0 || (y != headY && std::rand() % 1000 < FLICKER_PER_MILLE))
                            glyph = static_cast<uint16_t>(1 + std::rand() % chars.size());

                        uint8_t color;
                        if (y == headY)
                            color = C_HEAD;
                        else // 整數比較，避開浮點運算
                            color = ((headY - y) * 100 < tailLen * BODY_RATIO_PERCENT) ? C_BODY : C_TAIL;
                        curGrid[idx] = Cell{glyph, color};
                    } else {
                        glyph = 0; // 尾巴離開這格，字元消失
                        curGrid[idx] = Cell{};
                    }
                }
            }
        }

        // 7. 和上一幀比對，只輸出有變的格子
        if (needFullRedraw) {
            std::fill(prevGrid.begin(), prevGrid.end(), INVALID_CELL); // 讓每一格都被視為「有變」
            needFullRedraw = false;
        }

        // 開始同步更新（支援的終端機會等整幀收齊才一次顯示，減少撕裂；不支援的會直接忽略）
        frameBuffer = "\x1B[?2026h";

        const char* curColor = nullptr; // 目前終端機正在使用的顏色（nullptr = 預設）
        int lastY = -1, lastX = -2;     // 上一個輸出的格子位置，用來判斷游標是否已經在正確位置
        bool anyChange = false;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t idx = static_cast<size_t>(y) * width + x;
                const Cell& cell = curGrid[idx];
                if (cell == prevGrid[idx]) continue;

                anyChange = true;
                // 緊接在上一個輸出的格子後面時，游標自己會往右一格，不必再定位
                if (!(y == lastY && x == lastX + 1))
                    appendCursorPos(frameBuffer, y + 1, x + 1);

                if (cell.glyph == 0) {
                    frameBuffer += ' '; // 空白看不出前景色，不需要設色
                } else {
                    if (palette[cell.color] != curColor) {
                        curColor = palette[cell.color];
                        frameBuffer += curColor;
                    }
                    if (cell.glyph >= 0x100)
                        frameBuffer += static_cast<char>(cell.glyph & 0xFF);
                    else
                        frameBuffer += chars[cell.glyph - 1];
                }
                lastY = y;
                lastX = x;
            }
        }

        if (curColor) frameBuffer += COLOR_RESET; // 整幀結束才重置一次
        frameBuffer += "\x1B[?2026l";             // 結束同步更新

        // 這一幀就是螢幕上現在的樣子，下一幀拿它來比對（交換比複製快）
        prevGrid.swap(curGrid);

        // 直接用 WriteFile 輸出，繞過 CRT 的緩衝；單次上限 32KB，避免整幀重畫時單次寫入過大
        if (anyChange) {
            const char* p = frameBuffer.data();
            size_t remaining = frameBuffer.size();
            while (remaining > 0) {
                DWORD toWrite = static_cast<DWORD>(remaining > 32768 ? 32768 : remaining);
                DWORD written = 0;
                if (!WriteFile(hOut, p, toWrite, &written, nullptr) || written == 0) break;
                p += written;
                remaining -= written;
            }
        }

        // 更新雨滴座標：依「實際經過的時間」前進，而不是每幀固定走一格，
        // 所以不論終端機顯示得快或慢，雨滴的速度都一樣
        QueryPerformanceCounter(&qpcNow);
        float dt = static_cast<float>(qpcNow.QuadPart - prevCounter) / static_cast<float>(qpcFreq.QuadPart);
        prevCounter = qpcNow.QuadPart;
        if (dt > MAX_DT) dt = MAX_DT;

        const float baseStep = SPEED_LEVELS[speedLevel] * dt;
        for (int x = 0; x < width; ++x) {
            Drop& d = drops[x];
            d.pos += baseStep * d.speedFactor;
            if (d.pos - d.tailLen > height) {
                // 整條尾巴都離開畫面了：依目前密度決定下一滴多久之後出現
                initDrop(d, GAP_LEVELS[densityLevel]);
            }
        }

        // 睡到下一幀的預定時間；這一幀花多久就少睡多久，節奏才會穩
        QueryPerformanceCounter(&qpcNow);
        LONGLONG remain = nextFrame - qpcNow.QuadPart;
        if (remain > 0) {
            Sleep(static_cast<DWORD>(remain * 1000 / qpcFreq.QuadPart));
        } else {
            nextFrame = qpcNow.QuadPart; // 已經落後（例如視窗剛縮放），重新對齊，不連續補幀
        }
        nextFrame += frameTicks;
    }

    // 結束時恢復游標、顏色與主控台輸入模式
    std::cout << COLOR_RESET << "\x1B[?25h\x1B[2J\x1B[H" << std::flush;
    SetConsoleMode(hIn, origInMode);
    timeEndPeriod(1);
    return 0;
}