#include "Tuning.h"
#include "Gfx.h"
#include "Scene.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct TuningEntry
{
  const char* section;
  const char* key;
  float Tuning::*field;
  float defaultValue;
  float minValue;
  float maxValue;
};

const TuningEntry TUNING_TABLE[] = {
#define TUNING_TABLE_ROW(section, key, field, defaultValue, minValue, maxValue)                                                            \
  {section, key, &Tuning::field, defaultValue, minValue, maxValue},
    TUNING_FIELDS(TUNING_TABLE_ROW)
#undef TUNING_TABLE_ROW
};
constexpr int TUNING_COUNT = int(std::size(TUNING_TABLE));

// Layout, in 96-dpi units; every one of them goes through Scaled() before it reaches a control.
constexpr int TUNER_WIDTH = 486;
constexpr int TUNER_MAX_HEIGHT = 900;
constexpr int ROW_HEIGHT = 24;
constexpr int SECTION_HEIGHT = 32;
constexpr int NAME_WIDTH = 152;
constexpr int VALUE_WIDTH = 64;
constexpr int SLIDER_X = 232;
constexpr int SLIDER_WIDTH = TUNER_WIDTH - SLIDER_X - 26;
constexpr int BOTTOM_BAR = 44;
constexpr int SLIDER_STEPS = 1000;
constexpr int ID_SAVE = 100;
constexpr int ID_RELOAD = 101;
constexpr int ID_FIRST_SLIDER = 1000;
constexpr int ID_FIRST_VALUE = 5000;

struct Tuner
{
  HWND frame = nullptr;
  HWND viewport = nullptr; // clips the canvas so scrolled rows stay out of the button strip
  HWND canvas = nullptr;
  HWND saveButton = nullptr;
  HWND reloadButton = nullptr;
  HWND sliders[TUNING_COUNT] = {};
  HWND valueLabels[TUNING_COUNT] = {};
  HFONT font = nullptr;
  HFONT boldFont = nullptr;
  int contentHeightPx = 0;
  int scrollPx = 0;
  float scale = 1.0f;
};

// The tuning.ini watch. Overlapped and polled from the frame loop, so there is no watcher thread
// and nothing to synchronise.
struct Watch
{
  HANDLE dir = INVALID_HANDLE_VALUE;
  OVERLAPPED overlapped = {};
  alignas(DWORD) uint8_t buffer[8192] = {};
  bool pending = false;
  bool retry = false; // an editor caught mid-write reads as empty; try again next frame
};

Tuner g_tuner;
Watch g_watch;
std::wstring g_iniPath;

int Scaled(int _units)
{
  return int(float(_units) * g_tuner.scale + 0.5f);
}

std::wstring Widen(const char* _text)
{
  const int needed = MultiByteToWideChar(CP_UTF8, 0, _text, -1, nullptr, 0);
  if (needed <= 1)
  {
    return {};
  }
  std::wstring wide(size_t(needed - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _text, -1, wide.data(), needed);
  return wide;
}

std::string_view Trim(std::string_view _text)
{
  while (!_text.empty() && (_text.front() == ' ' || _text.front() == '\t' || _text.front() == '\r'))
  {
    _text.remove_prefix(1);
  }
  while (!_text.empty() && (_text.back() == ' ' || _text.back() == '\t' || _text.back() == '\r'))
  {
    _text.remove_suffix(1);
  }
  return _text;
}

std::string ReadTextFile(const std::wstring& _path)
{
  HANDLE file = CreateFileW(_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    return {};
  }
  LARGE_INTEGER size = {};
  GetFileSizeEx(file, &size);
  std::string text(size_t(size.QuadPart), '\0');
  DWORD read = 0;
  ReadFile(file, text.data(), DWORD(text.size()), &read, nullptr);
  text.resize(read);
  CloseHandle(file);
  return text;
}

// Returns false if nothing was understood, which is how a half-written file is detected.
bool LoadIni()
{
  const std::string text = ReadTextFile(g_iniPath);
  if (text.empty())
  {
    return false;
  }

  std::string section;
  int lineNumber = 0;
  int applied = 0;
  size_t start = 0;
  while (start <= text.size())
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
    {
      end = text.size();
    }
    const std::string_view line = Trim(std::string_view(text).substr(start, end - start));
    start = end + 1;
    ++lineNumber;

    if (line.empty() || line.front() == ';' || line.front() == '#')
    {
      continue;
    }
    if (line.front() == '[')
    {
      const size_t close = line.find(']');
      section.assign(close == std::string_view::npos ? line.substr(1) : line.substr(1, close - 1));
      continue;
    }

    const size_t equals = line.find('=');
    if (equals == std::string_view::npos)
    {
      DebugPrintf("tuning.ini(%d): expected 'key = value'\n", lineNumber);
      continue;
    }
    const std::string_view key = Trim(line.substr(0, equals));
    const std::string valueText(Trim(line.substr(equals + 1)));

    const TuningEntry* found = nullptr;
    for (const TuningEntry& entry : TUNING_TABLE)
    {
      if (section == entry.section && key == entry.key)
      {
        found = &entry;
        break;
      }
    }
    if (!found)
    {
      DebugPrintf("tuning.ini(%d): unknown value %s.%.*s\n", lineNumber, section.c_str(), int(key.size()), key.data());
      continue;
    }
    // Deliberately not clamped to the slider range: a value hand-edited past a slider bound is taken
    // as written.
    g_tuning.*found->field = float(std::atof(valueText.c_str()));
    ++applied;
  }
  return applied > 0;
}

} // namespace

void TuningSave()
{
  std::string text = "; ShipFeel tuning. Saved by the Tuner window, and reloaded while running whenever this\n"
                     "; file changes on disk -- so editing it by hand works just as well as dragging a slider.\n";

  const char* section = nullptr;
  char line[256] = {};
  for (const TuningEntry& entry : TUNING_TABLE)
  {
    if (!section || std::strcmp(section, entry.section) != 0)
    {
      section = entry.section;
      text += "\n[";
      text += section;
      text += "]\n";
    }
    std::snprintf(line, sizeof(line), "%-24s = %.6g\n", entry.key, double(g_tuning.*entry.field));
    text += line;
  }

  HANDLE file = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    DebugPrintf("cannot write %S\n", g_iniPath.c_str());
    return;
  }
  DWORD written = 0;
  WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr);
  CloseHandle(file);
  DebugPrintf("wrote %S (%d values)\n", g_iniPath.c_str(), TUNING_COUNT);
}

namespace
{

void IssueWatch()
{
  if (g_watch.dir == INVALID_HANDLE_VALUE)
  {
    return;
  }
  ResetEvent(g_watch.overlapped.hEvent);
  g_watch.pending = ReadDirectoryChangesW(g_watch.dir, g_watch.buffer, sizeof(g_watch.buffer), FALSE,
                                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &g_watch.overlapped,
                                          nullptr) != FALSE;
}

void StartWatch(const std::wstring& _directory)
{
  g_watch.dir = CreateFileW(_directory.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (g_watch.dir == INVALID_HANDLE_VALUE)
  {
    DebugPrintf("cannot watch %S for tuning changes\n", _directory.c_str());
    return;
  }
  g_watch.overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  IssueWatch();
}

bool PollWatch()
{
  if (!g_watch.pending || WaitForSingleObject(g_watch.overlapped.hEvent, 0) != WAIT_OBJECT_0)
  {
    return false;
  }
  DWORD bytes = 0;
  const BOOL ok = GetOverlappedResult(g_watch.dir, &g_watch.overlapped, &bytes, FALSE);
  g_watch.pending = false;

  // Zero bytes means the notification buffer overflowed, so assume the file is among what changed.
  bool touched = !ok || bytes == 0;
  const uint8_t* cursor = g_watch.buffer;
  while (ok && bytes != 0 && !touched)
  {
    const FILE_NOTIFY_INFORMATION* notify = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
    const std::wstring name(notify->FileName, notify->FileNameLength / sizeof(WCHAR));
    touched = _wcsicmp(name.c_str(), L"tuning.ini") == 0;
    if (notify->NextEntryOffset == 0)
    {
      break;
    }
    cursor += notify->NextEntryOffset;
  }

  IssueWatch();
  return touched;
}

int PosFromValue(const TuningEntry& _entry, float _value)
{
  const float span = _entry.maxValue - _entry.minValue;
  if (span <= 0.0f)
  {
    return 0;
  }
  return std::clamp(int((_value - _entry.minValue) / span * float(SLIDER_STEPS) + 0.5f), 0, SLIDER_STEPS);
}

float ValueFromPos(const TuningEntry& _entry, int _pos)
{
  return _entry.minValue + (float(_pos) / float(SLIDER_STEPS)) * (_entry.maxValue - _entry.minValue);
}

void SetValueLabel(int _index, float _value)
{
  wchar_t text[64] = {};
  std::swprintf(text, 64, L"%.4g", double(_value));
  SetWindowTextW(g_tuner.valueLabels[_index], text);
}

void RefreshControls()
{
  if (!g_tuner.canvas)
  {
    return;
  }
  for (int i = 0; i < TUNING_COUNT; ++i)
  {
    const float value = g_tuning.*TUNING_TABLE[i].field;
    SendMessageW(g_tuner.sliders[i], TBM_SETPOS, TRUE, LPARAM(PosFromValue(TUNING_TABLE[i], value)));
    SetValueLabel(i, value);
  }
}

void OnSliderMoved(HWND _slider)
{
  const int index = GetDlgCtrlID(_slider) - ID_FIRST_SLIDER;
  if (index < 0 || index >= TUNING_COUNT)
  {
    return;
  }
  const TuningEntry& entry = TUNING_TABLE[index];
  const float value = ValueFromPos(entry, int(SendMessageW(_slider, TBM_GETPOS, 0, 0)));
  g_tuning.*entry.field = value; // live, immediately
  SetValueLabel(index, value);
}

void SetScroll(int _scrollPx)
{
  RECT client = {};
  GetClientRect(g_tuner.frame, &client);
  const int viewportHeight = std::max(1, int(client.bottom) - Scaled(BOTTOM_BAR));
  g_tuner.scrollPx = std::clamp(_scrollPx, 0, std::max(0, g_tuner.contentHeightPx - viewportHeight));

  SCROLLINFO info = {};
  info.cbSize = sizeof(info);
  info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  info.nMin = 0;
  info.nMax = g_tuner.contentHeightPx;
  info.nPage = UINT(viewportHeight);
  info.nPos = g_tuner.scrollPx;
  SetScrollInfo(g_tuner.frame, SB_VERT, &info, TRUE);
  SetWindowPos(g_tuner.viewport, nullptr, 0, 0, int(client.right), viewportHeight, SWP_NOZORDER);
  SetWindowPos(g_tuner.canvas, nullptr, 0, -g_tuner.scrollPx, int(client.right), g_tuner.contentHeightPx, SWP_NOZORDER);
}

LRESULT CALLBACK TunerProc(HWND _hwnd, UINT _msg, WPARAM _wparam, LPARAM _lparam)
{
  if (_hwnd == g_tuner.canvas)
  {
    switch (_msg)
    {
    case WM_HSCROLL: // every trackbar reports here, since the canvas is their parent
      OnSliderMoved(reinterpret_cast<HWND>(_lparam));
      return 0;
    case WM_MOUSEWHEEL:
      return SendMessageW(g_tuner.frame, _msg, _wparam, _lparam);
    case WM_CTLCOLORSTATIC:
      SetBkMode(reinterpret_cast<HDC>(_wparam), TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    default:
      break;
    }
    return DefWindowProcW(_hwnd, _msg, _wparam, _lparam);
  }
  if (_hwnd != g_tuner.frame) // still inside CreateWindowExW for the frame itself
  {
    return DefWindowProcW(_hwnd, _msg, _wparam, _lparam);
  }

  switch (_msg)
  {
  case WM_VSCROLL:
  {
    SCROLLINFO info = {};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    GetScrollInfo(_hwnd, SB_VERT, &info);
    int scroll = g_tuner.scrollPx;
    switch (LOWORD(_wparam))
    {
    case SB_LINEUP: scroll -= Scaled(ROW_HEIGHT); break;
    case SB_LINEDOWN: scroll += Scaled(ROW_HEIGHT); break;
    case SB_PAGEUP: scroll -= int(info.nPage); break;
    case SB_PAGEDOWN: scroll += int(info.nPage); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: scroll = info.nTrackPos; break;
    default: break;
    }
    SetScroll(scroll);
    return 0;
  }

  case WM_MOUSEWHEEL:
    SetScroll(g_tuner.scrollPx - GET_WHEEL_DELTA_WPARAM(_wparam) * Scaled(ROW_HEIGHT * 3) / WHEEL_DELTA);
    return 0;

  case WM_SIZE:
  {
    if (!g_tuner.saveButton)
    {
      return 0;
    }
    const int height = HIWORD(_lparam);
    MoveWindow(g_tuner.saveButton, Scaled(12), height - Scaled(34), Scaled(94), Scaled(26), TRUE);
    MoveWindow(g_tuner.reloadButton, Scaled(114), height - Scaled(34), Scaled(94), Scaled(26), TRUE);
    SetScroll(g_tuner.scrollPx);
    return 0;
  }

  case WM_COMMAND:
    if (LOWORD(_wparam) == ID_SAVE)
    {
      TuningSave();
    }
    else if (LOWORD(_wparam) == ID_RELOAD)
    {
      if (LoadIni())
      {
        RefreshControls();
      }
    }
    return 0;

  case WM_CTLCOLORSTATIC:
    SetBkMode(reinterpret_cast<HDC>(_wparam), TRANSPARENT);
    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));

  case WM_CLOSE: // a tool window hides rather than closing; F2 brings it back
    ShowWindow(_hwnd, SW_HIDE);
    return 0;

  default:
    break;
  }
  return DefWindowProcW(_hwnd, _msg, _wparam, _lparam);
}

// One trackbar, one name and one live number per float in the table, grouped by ini section.
void BuildControls(HINSTANCE _instance)
{
  int y = Scaled(4);
  const char* section = nullptr;
  for (int i = 0; i < TUNING_COUNT; ++i)
  {
    const TuningEntry& entry = TUNING_TABLE[i];
    if (!section || std::strcmp(section, entry.section) != 0)
    {
      section = entry.section;
      const std::wstring caption = Widen((std::string("[") + section + "]").c_str());
      HWND header = CreateWindowExW(0, L"STATIC", caption.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, Scaled(12), y + Scaled(10),
                                    Scaled(TUNER_WIDTH - 30), Scaled(18), g_tuner.canvas, nullptr, _instance, nullptr);
      SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(g_tuner.boldFont), TRUE);
      y += Scaled(SECTION_HEIGHT);
    }

    const std::wstring name = Widen(entry.key);
    HWND nameLabel = CreateWindowExW(0, L"STATIC", name.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, Scaled(16), y + Scaled(4),
                                     Scaled(NAME_WIDTH), Scaled(16), g_tuner.canvas, nullptr, _instance, nullptr);
    g_tuner.valueLabels[i] =
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT, Scaled(16 + NAME_WIDTH), y + Scaled(4), Scaled(VALUE_WIDTH),
                        Scaled(16), g_tuner.canvas, reinterpret_cast<HMENU>(INT_PTR(ID_FIRST_VALUE + i)), _instance, nullptr);
    g_tuner.sliders[i] = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, Scaled(SLIDER_X), y,
                                         Scaled(SLIDER_WIDTH), Scaled(ROW_HEIGHT - 2), g_tuner.canvas,
                                         reinterpret_cast<HMENU>(INT_PTR(ID_FIRST_SLIDER + i)), _instance, nullptr);

    SendMessageW(nameLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_tuner.font), TRUE);
    SendMessageW(g_tuner.valueLabels[i], WM_SETFONT, reinterpret_cast<WPARAM>(g_tuner.font), TRUE);
    SendMessageW(g_tuner.sliders[i], TBM_SETRANGE, TRUE, LPARAM(MAKELONG(0, SLIDER_STEPS)));
    SendMessageW(g_tuner.sliders[i], TBM_SETPAGESIZE, 0, SLIDER_STEPS / 20);
    y += Scaled(ROW_HEIGHT);
  }
  g_tuner.contentHeightPx = y + Scaled(8);
}

void CreateTunerWindow(HINSTANCE _instance, HWND _mainWindow)
{
  INITCOMMONCONTROLSEX common = {};
  common.dwSize = sizeof(common);
  common.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&common);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TunerProc;
  wc.hInstance = _instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"ShipFeelTuner";
  RegisterClassExW(&wc);

  RECT owner = {};
  GetWindowRect(_mainWindow, &owner);
  g_tuner.scale = float(GetDpiForWindow(_mainWindow)) / 96.0f;

  const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_VSCROLL | WS_CLIPCHILDREN;
  RECT frameRect = {0, 0, Scaled(TUNER_WIDTH), Scaled(TUNER_MAX_HEIGHT)};
  AdjustWindowRectExForDpi(&frameRect, style, FALSE, WS_EX_TOOLWINDOW, GetDpiForWindow(_mainWindow));
  g_tuner.frame = CreateWindowExW(WS_EX_TOOLWINDOW, L"ShipFeelTuner", L"Tuner", style, owner.right + 8, owner.top,
                                  frameRect.right - frameRect.left, frameRect.bottom - frameRect.top, _mainWindow, nullptr, _instance,
                                  nullptr);
  if (!g_tuner.frame)
  {
    DebugPrintf("could not create the Tuner window\n");
    return;
  }

  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, GetDpiForWindow(g_tuner.frame));
  g_tuner.font = CreateFontIndirectW(&metrics.lfMessageFont);
  metrics.lfMessageFont.lfWeight = FW_BOLD;
  g_tuner.boldFont = CreateFontIndirectW(&metrics.lfMessageFont);

  RECT client = {};
  GetClientRect(g_tuner.frame, &client);
  g_tuner.viewport = CreateWindowExW(0, L"ShipFeelTuner", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, int(client.right),
                                     int(client.bottom) - Scaled(BOTTOM_BAR), g_tuner.frame, nullptr, _instance, nullptr);
  HWND canvas = CreateWindowExW(0, L"ShipFeelTuner", L"", WS_CHILD | WS_VISIBLE, 0, 0, int(client.right), Scaled(10), g_tuner.viewport,
                                nullptr, _instance, nullptr);
  g_tuner.canvas = canvas; // only now will TunerProc route messages to the canvas branch

  g_tuner.saveButton = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, Scaled(12), Scaled(10), Scaled(94),
                                       Scaled(26), g_tuner.frame, reinterpret_cast<HMENU>(INT_PTR(ID_SAVE)), _instance, nullptr);
  g_tuner.reloadButton = CreateWindowExW(0, L"BUTTON", L"Reload", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, Scaled(114), Scaled(10),
                                         Scaled(94), Scaled(26), g_tuner.frame, reinterpret_cast<HMENU>(INT_PTR(ID_RELOAD)), _instance,
                                         nullptr);
  SendMessageW(g_tuner.saveButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_tuner.font), TRUE);
  SendMessageW(g_tuner.reloadButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_tuner.font), TRUE);

  BuildControls(_instance);
  RefreshControls();

  GetClientRect(g_tuner.frame, &client);
  SendMessageW(g_tuner.frame, WM_SIZE, 0, LPARAM(MAKELONG(client.right, client.bottom)));
  ShowWindow(g_tuner.frame, SW_SHOWNOACTIVATE);
}

} // namespace

void TuningInit(HINSTANCE _instance, HWND _mainWindow)
{
  const std::wstring root = FindDataRoot();
  g_iniPath = root + L"tuning.ini";

  if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
  {
    DebugPrintf("no tuning.ini yet; writing the defaults\n");
    TuningSave();
  }
  else if (!LoadIni())
  {
    DebugPrintf("tuning.ini had nothing usable in it; keeping the defaults\n");
  }

  std::wstring watchDir = root;
  while (watchDir.size() > 3 && watchDir.back() == L'\\')
  {
    watchDir.pop_back();
  }
  StartWatch(watchDir);
  CreateTunerWindow(_instance, _mainWindow);
}

void TuningPoll()
{
  if (!PollWatch() && !g_watch.retry)
  {
    return;
  }
  g_watch.retry = !LoadIni(); // an editor caught mid-write; come back next frame
  if (!g_watch.retry)
  {
    RefreshControls();
    DebugPrintf("tuning.ini reloaded\n");
  }
}

void TuningToggleWindow()
{
  if (!g_tuner.frame)
  {
    return;
  }
  const bool visible = IsWindowVisible(g_tuner.frame) != FALSE;
  ShowWindow(g_tuner.frame, visible ? SW_HIDE : SW_SHOWNOACTIVATE);
}

void TuningShutdown()
{
  if (g_watch.dir != INVALID_HANDLE_VALUE)
  {
    CancelIoEx(g_watch.dir, &g_watch.overlapped);
    CloseHandle(g_watch.dir);
    g_watch.dir = INVALID_HANDLE_VALUE;
  }
  if (g_watch.overlapped.hEvent)
  {
    CloseHandle(g_watch.overlapped.hEvent);
    g_watch.overlapped.hEvent = nullptr;
  }
  if (g_tuner.font)
  {
    DeleteObject(g_tuner.font);
  }
  if (g_tuner.boldFont)
  {
    DeleteObject(g_tuner.boldFont);
  }
}
