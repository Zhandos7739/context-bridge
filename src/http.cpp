// Copyright (c) 2026 ProtocolTeam (Zhandos). All rights reserved.
// Viewing allowed. No copying, modification, commercial use, or redistribution
// without written permission from ProtocolTeam (Zhandos).

#include "http.hpp"

#include "utf.hpp"

#include <algorithm>
#include <stdexcept>
#include <winhttp.h>

namespace {

std::string status_error(DWORD status, const std::string& body) {
    std::string snippet = body.substr(0, std::min<size_t>(body.size(), 300));
    return "HTTP " + std::to_string(status) + (snippet.empty() ? "" : ": " + snippet);
}

}  // namespace

std::string http_post(const std::string& url, const std::string& body, const std::string& api_key) {
    std::wstring wide = utf8_to_wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4095;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2047;
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) {
        throw std::runtime_error("Некорректный адрес API.");
    }
    HINTERNET session = WinHttpOpen(L"ContextBridge/0.1 ProtocolTeam", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHTTP не открылся.");
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 120000);
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("Нет соединения с API.");
    }
    std::wstring object(path);
    object += extra;
    if (object.empty()) object = L"/";
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("Не удалось создать запрос.");
    }
    std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n";
    if (!api_key.empty()) headers += L"Authorization: Bearer " + utf8_to_wide(api_key) + L"\r\n";
    BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                 const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                 static_cast<DWORD>(body.size()), 0);
    if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("Запрос к модели не выполнен. Проверьте адрес и сеть.");
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) break;
        if (available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        response += chunk;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (status >= 400) throw std::runtime_error(status_error(status, response));
    return response;
}
