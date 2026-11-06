// Copyright (c) 2026 ProtocolTeam (Zhandos). All rights reserved.
// Viewing allowed. No copying, modification, commercial use, or redistribution
// without written permission from ProtocolTeam (Zhandos).

#include "bridge.hpp"
#include "http.hpp"
#include "utf.hpp"

#include <commctrl.h>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <shellapi.h>
#include <stdexcept>
#include <string>

namespace {

enum {
    IDC_ADAPTER = 1001,
    IDC_MODEL,
    IDC_URL,
    IDC_KEY,
    IDC_MAX,
    IDC_THRESH,
    IDC_APPLY,
    IDC_COMPACT,
    IDC_FOLDER,
    IDC_LOG,
    IDC_INPUT,
    IDC_SEND,
    IDC_FILES,
    IDC_STATUS
};

constexpr UINT WM_TURN_DONE = WM_APP + 1;

struct Done {
    TurnResult result;
    std::string error;
};

struct App {
    HWND hwnd{};
    HWND adapter{};
    HWND model{};
    HWND url{};
    HWND key{};
    HWND max_tokens{};
    HWND threshold{};
    HWND log{};
    HWND input{};
    HWND files{};
    HWND status{};
    HWND send{};
    HFONT font{};
    std::mutex mu;
    Bridge* bridge = nullptr;
    AdapterKind kind = AdapterKind::Local;
    std::string model_name;
    std::string base_url;
    std::string api_key;
    std::filesystem::path memory;
    std::string pending;
    bool busy = false;
};

App* g_app = nullptr;

const wchar_t* kDefaults[] = {
    L"",
    L"https://api.openai.com/v1",
    L"http://127.0.0.1:11434",
    L"http://127.0.0.1:8080/v1",
    L"https://router.huggingface.co/v1",
};

std::wstring window_text(HWND hwnd) {
    int n = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(hwnd, text.data(), n + 1);
    return text;
}

void set_status(App* app, const std::wstring& text) { SetWindowTextW(app->status, text.c_str()); }

std::wstring usage_line(const Bridge& bridge) {
    int tokens = bridge.prompt_tokens();
    int budget = bridge.prompt_budget();
    std::wstring line = L"Окно " + std::to_wstring(tokens) + L"/" + std::to_wstring(budget) + L" ток.  порог " +
                        std::to_wstring(bridge.threshold_tokens()) + L"  файлов " +
                        std::to_wstring(bridge.store.list_segments().size()) + L"  указателей " +
                        std::to_wstring(bridge.pointers().size());
    if (!bridge.warning.empty()) line += L"  " + utf8_to_wide(bridge.warning);
    return line;
}

std::wstring transcript(const Bridge& bridge) {
    std::wstring out;
    for (const auto& message : bridge.messages) {
        if (message.kind == "protocol") continue;
        if (message.kind == "pointer") {
            out += L"Указатель\r\n" + utf8_to_wide(message.content) + L"\r\n\r\n";
            continue;
        }
        if (message.role == "user") out += L"Вы\r\n";
        else if (message.role == "assistant") out += L"Модель\r\n";
        else out += utf8_to_wide(message.role) + L"\r\n";
        out += utf8_to_wide(message.content) + L"\r\n\r\n";
    }
    if (out.empty()) {
        out = L"Окно пустое. Напишите сообщение или нажмите «Сжать», когда диалог станет длинным.\r\n"
              L"Старый текст уйдёт в нумерованный файл, здесь останется строка file N: …\r\n";
    }
    return out;
}

void refresh(App* app) {
    std::lock_guard<std::mutex> lock(app->mu);
    SetWindowTextW(app->log, transcript(*app->bridge).c_str());
    SendMessageW(app->log, EM_SETSEL, GetWindowTextLengthW(app->log), GetWindowTextLengthW(app->log));
    SendMessageW(app->log, EM_SCROLLCARET, 0, 0);
    SendMessageW(app->files, LB_RESETCONTENT, 0, 0);
    for (const auto& segment : app->bridge->store.list_segments()) {
        std::wstring item = L"file " + utf8_to_wide(segment.id) + L"  " + utf8_to_wide(segment.summary);
        SendMessageW(app->files, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    set_status(app, usage_line(*app->bridge));
}

void save_settings(App* app) {
    std::ofstream out(app->memory / "settings.txt", std::ios::binary | std::ios::trunc);
    out << "adapter=" << static_cast<int>(app->kind) << "\n";
    out << "model=" << app->model_name << "\n";
    out << "url=" << app->base_url << "\n";
    out << "key=" << app->api_key << "\n";
    out << "max=" << app->bridge->config.max_context_tokens << "\n";
    out << "threshold=" << app->bridge->config.compact_threshold << "\n";
}

void load_settings(App* app) {
    std::ifstream in(app->memory / "settings.txt");
    if (!in) return;
    try {
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "adapter") {
            int n = std::stoi(value);
            if (n >= 0 && n <= 4) app->kind = static_cast<AdapterKind>(n);
        } else if (key == "model") app->model_name = value;
        else if (key == "url") app->base_url = value;
        else if (key == "key") app->api_key = value;
        else if (key == "max") app->bridge->config.max_context_tokens = std::stoi(value);
        else if (key == "threshold") app->bridge->config.compact_threshold = std::stod(value);
    }
    } catch (...) {
    }
}

void push_fields(App* app) {
    SendMessageW(app->adapter, CB_SETCURSEL, static_cast<WPARAM>(app->kind), 0);
    SetWindowTextW(app->model, utf8_to_wide(app->model_name).c_str());
    SetWindowTextW(app->url, utf8_to_wide(app->base_url).c_str());
    SetWindowTextW(app->key, utf8_to_wide(app->api_key).c_str());
    SetWindowTextW(app->max_tokens, std::to_wstring(app->bridge->config.max_context_tokens).c_str());
    wchar_t buf[32];
    swprintf(buf, 32, L"%.2f", app->bridge->config.compact_threshold);
    SetWindowTextW(app->threshold, buf);
}

bool read_fields(App* app, std::wstring& error) {
    int sel = static_cast<int>(SendMessageW(app->adapter, CB_GETCURSEL, 0, 0));
    if (sel < 0) sel = 0;
    app->kind = static_cast<AdapterKind>(sel);
    app->model_name = wide_to_utf8(window_text(app->model));
    app->base_url = wide_to_utf8(window_text(app->url));
    app->api_key = wide_to_utf8(window_text(app->key));
    try {
        int max_tokens = std::stoi(wide_to_utf8(window_text(app->max_tokens)));
        double threshold = std::stod(wide_to_utf8(window_text(app->threshold)));
        if (max_tokens < 64) {
            error = L"Окно должно быть не меньше 64 токенов.";
            return false;
        }
        if (threshold <= 0 || threshold > 1) {
            error = L"Порог — число от 0 до 1, например 0.75.";
            return false;
        }
        std::lock_guard<std::mutex> lock(app->mu);
        app->bridge->config.max_context_tokens = max_tokens;
        app->bridge->config.compact_threshold = threshold;
        if (app->bridge->config.reserve_tokens >= max_tokens) app->bridge->config.reserve_tokens = std::max(32, max_tokens / 5);
        std::vector<TurnEvent> events;
        app->bridge->ensure_budget(&events);
        app->bridge->save();
    } catch (...) {
        error = L"Не удалось прочитать числа окна и порога.";
        return false;
    }
    save_settings(app);
    return true;
}

void set_busy(App* app, bool busy) {
    app->busy = busy;
    EnableWindow(app->send, !busy);
    EnableWindow(GetDlgItem(app->hwnd, IDC_APPLY), !busy);
    EnableWindow(GetDlgItem(app->hwnd, IDC_COMPACT), !busy);
}

DWORD WINAPI worker(LPVOID param) {
    auto* app = static_cast<App*>(param);
    Done* done = new Done();
    std::string user = app->pending;
    try {
        std::lock_guard<std::mutex> lock(app->mu);
        done->result = app->bridge->run_turn(user, [&](const std::vector<Message>& messages) {
            try {
                if (app->kind == AdapterKind::Local) return local_reply(messages);
                if (app->model_name.empty()) {
                    ModelReply reply;
                    reply.error = "Укажите имя модели.";
                    return reply;
                }
                std::string url = endpoint_for(app->kind, app->base_url);
                std::string body = request_body(app->kind, app->model_name, messages, true);
                std::string response = http_post(url, body, app->api_key);
                return parse_response(app->kind, response);
            } catch (const std::exception& ex) {
                ModelReply reply;
                reply.error = ex.what();
                return reply;
            }
        });
    } catch (const std::exception& ex) {
        done->error = ex.what();
    }
    PostMessageW(app->hwnd, WM_TURN_DONE, 0, reinterpret_cast<LPARAM>(done));
    return 0;
}

void send_chat(App* app) {
    if (app->busy) return;
    if (window_text(app->input).empty()) return;
    std::wstring error;
    if (!read_fields(app, error)) {
        set_status(app, error);
        return;
    }
    set_busy(app, true);
    set_status(app, L"Мост собирает окно и ждёт модель…");
    app->pending = wide_to_utf8(window_text(app->input));
    HANDLE thread = CreateThread(nullptr, 0, worker, app, 0, nullptr);
    if (thread) CloseHandle(thread);
    else set_busy(app, false);
}

void layout(App* app) {
    RECT rc{};
    GetClientRect(app->hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    int m = 8;
    int row = 28;
    int y = m;
    int files_w = w > 900 ? 280 : 0;
    MoveWindow(app->adapter, m, y, 190, 200, TRUE);
    MoveWindow(app->model, 204, y, 180, row, TRUE);
    MoveWindow(app->url, 390, y, std::max(80, w - 390 - 200 - m), row, TRUE);
    MoveWindow(app->key, w - 190 - m, y, 190, row, TRUE);
    y += row + 6;
    MoveWindow(app->max_tokens, m, y, 80, row, TRUE);
    MoveWindow(app->threshold, 94, y, 70, row, TRUE);
    MoveWindow(GetDlgItem(app->hwnd, IDC_APPLY), 172, y, 110, row, TRUE);
    MoveWindow(GetDlgItem(app->hwnd, IDC_COMPACT), 288, y, 90, row, TRUE);
    MoveWindow(GetDlgItem(app->hwnd, IDC_FOLDER), 384, y, 110, row, TRUE);
    y += row + 6;
    int status_h = 22;
    int input_h = 28;
    int bottom = h - m - input_h - status_h - 8;
    int log_w = std::max(120, w - m * 2 - files_w - (files_w ? 8 : 0));
    MoveWindow(app->log, m, y, log_w, std::max(80, bottom - y), TRUE);
    if (files_w) MoveWindow(app->files, m + log_w + 8, y, files_w, std::max(80, bottom - y), TRUE);
    else MoveWindow(app->files, 0, 0, 0, 0, TRUE);
    MoveWindow(app->input, m, bottom + 4, w - m * 2 - 110, input_h, TRUE);
    MoveWindow(app->send, w - m - 100, bottom + 4, 100, input_h, TRUE);
    MoveWindow(app->status, m, h - m - status_h, w - m * 2, status_h, TRUE);
}

void show_selected_file(App* app) {
    int sel = static_cast<int>(SendMessageW(app->files, LB_GETCURSEL, 0, 0));
    if (sel == LB_ERR) return;
    std::vector<Segment> segments;
    {
        std::lock_guard<std::mutex> lock(app->mu);
        segments = app->bridge->store.list_segments();
    }
    if (sel < 0 || sel >= static_cast<int>(segments.size())) return;
    std::wstring body = utf8_to_wide(render_segment_text(segments[static_cast<size_t>(sel)]));
    if (body.size() > 2500) body.resize(2500);
    MessageBoxW(app->hwnd, body.c_str(), L"Оригинал", MB_OK);
}

LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN && wparam == VK_RETURN) {
        SendMessageW(reinterpret_cast<HWND>(ref), WM_COMMAND, MAKEWPARAM(IDC_SEND, BN_CLICKED), 0);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* created = reinterpret_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            created->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            g_app = created;
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            };
            created->adapter = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_ADAPTER);
            const wchar_t* names[] = {L"Локально, без сети", L"OpenAI-совместимый", L"Ollama", L"llama.cpp", L"Hugging Face"};
            for (auto* name : names) SendMessageW(created->adapter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            created->model = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, IDC_MODEL);
            created->url = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, IDC_URL);
            created->key = make(L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, IDC_KEY);
            created->max_tokens = make(L"EDIT", L"4096", WS_BORDER | ES_NUMBER, IDC_MAX);
            created->threshold = make(L"EDIT", L"0.75", WS_BORDER, IDC_THRESH);
            make(L"BUTTON", L"Применить", BS_PUSHBUTTON, IDC_APPLY);
            make(L"BUTTON", L"Сжать", BS_PUSHBUTTON, IDC_COMPACT);
            make(L"BUTTON", L"Папка архива", BS_PUSHBUTTON, IDC_FOLDER);
            created->log = make(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, IDC_LOG);
            created->files = make(L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | WS_VSCROLL, IDC_FILES);
            created->input = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, IDC_INPUT);
            created->send = make(L"BUTTON", L"Отправить", BS_DEFPUSHBUTTON, IDC_SEND);
            created->status = make(L"STATIC", L"", SS_LEFT, IDC_STATUS);
            created->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            EnumChildWindows(hwnd, [](HWND child, LPARAM font) {
                SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
                return TRUE;
            }, reinterpret_cast<LPARAM>(created->font));
            SetWindowSubclass(created->input, input_proc, 1, reinterpret_cast<DWORD_PTR>(hwnd));
            load_settings(created);
            push_fields(created);
            refresh(created);
            return 0;
        }
        case WM_SIZE:
            if (app) layout(app);
            return 0;
        case WM_COMMAND: {
            if (!app) break;
            int id = LOWORD(wparam);
            int code = HIWORD(wparam);
            if (id == IDC_ADAPTER && code == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(app->adapter, CB_GETCURSEL, 0, 0));
                std::wstring current = window_text(app->url);
                bool known = current.empty();
                for (auto* item : kDefaults) {
                    if (current == item) known = true;
                }
                if (known && sel >= 0 && sel <= 4) SetWindowTextW(app->url, kDefaults[sel]);
            } else if (id == IDC_SEND && code == BN_CLICKED) {
                send_chat(app);
            } else if (id == IDC_APPLY && code == BN_CLICKED) {
                std::wstring error;
                if (!read_fields(app, error)) set_status(app, error);
                else refresh(app);
            } else if (id == IDC_COMPACT && code == BN_CLICKED) {
                std::vector<TurnEvent> events;
                {
                    std::lock_guard<std::mutex> lock(app->mu);
                    if (!app->bridge->dump_once(&events, true, true)) set_status(app, L"Сжимать нечего.");
                    else app->bridge->save();
                }
                refresh(app);
            } else if (id == IDC_FOLDER && code == BN_CLICKED) {
                ShellExecuteW(hwnd, L"open", app->memory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (id == IDC_FILES && code == LBN_DBLCLK) {
                show_selected_file(app);
            }
            return 0;
        }
        case WM_TURN_DONE: {
            auto* done = reinterpret_cast<Done*>(lparam);
            if (app) {
                set_busy(app, false);
                SetWindowTextW(app->input, L"");
                refresh(app);
                if (!done->error.empty()) set_status(app, utf8_to_wide(done->error));
                else if (!done->result.error.empty()) set_status(app, utf8_to_wide(done->result.error));
                else {
                    std::wstring line = usage_line(*app->bridge);
                    for (const auto& event : done->result.events) {
                        if (event.type == "compacted") line += L"  сжато в file " + utf8_to_wide(event.file_id);
                        if (event.type == "read") line += L"  прочитан file " + utf8_to_wide(event.file_id);
                    }
                    set_status(app, line);
                }
            }
            delete done;
            return 0;
        }
        case WM_DESTROY:
            if (app) {
                std::wstring error;
                read_fields(app, error);
                if (app->font) DeleteObject(app->font);
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path memory = std::filesystem::path(exe).parent_path() / L"memory";
    std::filesystem::create_directories(memory);
    Config config;
    App app;
    app.memory = memory;
    app.bridge = new Bridge(Store(memory), config);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ContextBridgeWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Кортекс — мост контекста    ProtocolTeam (Zhandos)",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, nullptr, nullptr, instance, &app);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    delete app.bridge;
    return 0;
}
