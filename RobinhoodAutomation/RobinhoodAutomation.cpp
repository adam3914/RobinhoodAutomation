#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>
#include <atomic>
#include <chrono>
#include <thread>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

const std::string TOKEN_FILE = "rh_tokens.json";
const std::string REDIRECT_URI = "http://127.0.0.1:8400/callback";
const int CALLBACK_PORT = 8400;

// --- String & JSON Parsing Helpers ---

std::wstring ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], sz);
    return wstr;
}

std::string UrlEncode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::ostringstream escaped;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        }
        else {
            escaped << '%' << hex[c >> 4] << hex[c & 0x0F];
        }
    }
    return escaped.str();
}

std::string Base64UrlEncode(const unsigned char* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    return out;
}

std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    size_t end = pos + 1;
    while (end < json.size()) {
        if (json[end] == '"' && json[end - 1] != '\\') break;
        end++;
    }
    return json.substr(pos + 1, end - pos - 1);
}

bool ParseJsonString(const std::string& json, size_t quotePos, std::string& value, size_t& nextPos) {
    if (quotePos >= json.size() || json[quotePos] != '"') return false;

    value.clear();
    for (size_t i = quotePos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') {
            nextPos = i + 1;
            return true;
        }
        if (c != '\\') {
            value.push_back(c);
            continue;
        }
        if (++i >= json.size()) return false;
        switch (json[i]) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u':
            if (i + 4 >= json.size()) return false;
            value.append(json, i - 1, 6);
            i += 4;
            break;
        default: return false;
        }
    }
    return false;
}

std::string FindLastJsonStringValue(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    std::string result;
    size_t searchFrom = 0;
    size_t pos = std::string::npos;

    while ((pos = json.find(pattern, searchFrom)) != std::string::npos) {
        searchFrom = pos + pattern.size();

        size_t previous = pos;
        while (previous > 0 && std::isspace(static_cast<unsigned char>(json[previous - 1]))) --previous;
        if (previous == 0 || (json[previous - 1] != '{' && json[previous - 1] != ',')) continue;

        size_t valuePos = searchFrom;
        while (valuePos < json.size() && std::isspace(static_cast<unsigned char>(json[valuePos]))) ++valuePos;
        if (valuePos >= json.size() || json[valuePos] != ':') continue;
        ++valuePos;
        while (valuePos < json.size() && std::isspace(static_cast<unsigned char>(json[valuePos]))) ++valuePos;
        if (valuePos >= json.size() || json[valuePos] != '"') continue;

        std::string parsed;
        size_t afterValue = 0;
        if (ParseJsonString(json, valuePos, parsed, afterValue)) {
            result = parsed;
            searchFrom = afterValue;
        }
    }
    return result;
}

void PrintStatusSummary(const std::string& jsonResponse, DWORD httpStatus) {
    std::cout << "\n========================================\n";
    std::cout << "          TRADING STATUS SUMMARY        \n";
    std::cout << "========================================\n";
    std::cout << "HTTP status: " << httpStatus << "\n";

    if (httpStatus == 0) {
        std::cout << "No HTTP response was received. Check the network connection and try again.\n";
        std::cout << "========================================\n";
        return;
    }

    const std::string responseStatus = FindLastJsonStringValue(jsonResponse, "status");
    if (!responseStatus.empty()) {
        std::cout << "Response status: " << responseStatus << "\n";
    }

    if (httpStatus < 200 || httpStatus >= 300) {
        const std::string errorMessage = FindLastJsonStringValue(jsonResponse, "message");
        std::cout << "Provider request failed";
        if (!errorMessage.empty()) std::cout << ": " << errorMessage;
        std::cout << "\n========================================\n";
        return;
    }

    const std::string summary = FindLastJsonStringValue(jsonResponse, "text");
    if (summary.empty()) {
        std::cout << "No assistant text was found in the response. Check the response status and provider tool configuration.\n";
        std::cout << "========================================\n";
        return;
    }

    std::cout << summary << "\n";
    std::cout << "========================================\n";
}

std::string GetEnvVar(const char* name) {
    char* buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, name) == 0 && buf != nullptr) {
        std::string raw(buf);
        free(buf);
        std::string clean;
        for (char c : raw) {
            if (c != ' ' && c != '"' && c != '\'' && c != '\r' && c != '\n' && c != '\t') {
                clean.push_back(c);
            }
        }
        return clean;
    }
    return "";
}

void GeneratePkce(std::string& verifier, std::string& challenge) {
    unsigned char rnd[32];
    BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    verifier = Base64UrlEncode(rnd, sizeof(rnd));

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    unsigned char hash[32];
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hHash, (PUCHAR)verifier.data(), (ULONG)verifier.size(), 0);
    BCryptFinishHash(hHash, hash, sizeof(hash), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    challenge = Base64UrlEncode(hash, sizeof(hash));
}

std::string HttpRequest(const std::wstring& host, const std::wstring& path,
    const std::wstring& method, const std::wstring& headers,
    const std::string& body = "", DWORD* outStatus = nullptr) {
    HINTERNET hSession = WinHttpOpen(L"RobinhoodCPPClient/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        std::cerr << "WinHTTP session creation failed (Windows error " << GetLastError() << ").\n";
        return "";
    }
    WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 280000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        std::cerr << "WinHTTP connect failed (Windows error " << GetLastError() << ").\n";
        WinHttpCloseHandle(hSession);
        return "";
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        std::cerr << "WinHTTP request creation failed (Windows error " << GetLastError() << ").\n";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    BOOL ok = WinHttpSendRequest(hRequest,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : -1L,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str(),
        (DWORD)body.size(), (DWORD)body.size(), 0);

    std::string response;
    if (!ok) {
        std::cerr << "WinHTTP send failed (Windows error " << GetLastError() << ").\n";
    }
    else if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        std::cerr << "WinHTTP response wait failed (Windows error " << GetLastError() << ").\n";
    }
    else {
        if (outStatus) {
            DWORD sz = sizeof(DWORD);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, outStatus, &sz, WINHTTP_NO_HEADER_INDEX);
        }
        DWORD dwSize = 0, dwDownloaded = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
                std::cerr << "WinHTTP response read setup failed (Windows error " << GetLastError() << ").\n";
                break;
            }
            if (!dwSize) break;
            std::string buf(dwSize, '\0');
            if (!WinHttpReadData(hRequest, &buf[0], dwSize, &dwDownloaded)) {
                std::cerr << "WinHTTP response body read failed (Windows error " << GetLastError() << ").\n";
                break;
            }
            response.append(buf, 0, dwDownloaded);
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

std::string HttpRequestWithProgress(const std::wstring& host, const std::wstring& path,
    const std::wstring& method, const std::wstring& headers,
    const std::string& body, DWORD* outStatus) {
    std::string response;
    DWORD status = 0;
    std::atomic<bool> completed(false);
    const auto start = std::chrono::steady_clock::now();

    std::thread requestThread([&]() {
        response = HttpRequest(host, path, method, headers, body, &status);
        completed.store(true, std::memory_order_release);
    });

    while (!completed.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        if (!completed.load(std::memory_order_acquire)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << "Still waiting for the provider/MCP response (" << elapsed
                << " seconds elapsed)...\n" << std::flush;
        }
    }

    requestThread.join();
    if (outStatus) *outStatus = status;
    return response;
}

std::string WaitForOAuthCallbackCode() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(CALLBACK_PORT);

    bind(listenSock, (sockaddr*)&addr, sizeof(addr));
    listen(listenSock, 1);

    std::cout << "Waiting for Robinhood login & mobile app approval in your browser...\n";
    SOCKET clientSock = accept(listenSock, nullptr, nullptr);

    char buffer[4096] = { 0 };
    recv(clientSock, buffer, sizeof(buffer) - 1, 0);
    std::string req(buffer);

    const char* html =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
        "<html><body style='font-family:sans-serif;text-align:center;margin-top:50px;'>"
        "<h2>Robinhood MCP Connected!</h2><p>You can close this tab and return to your C++ program.</p>"
        "</body></html>";
    send(clientSock, html, (int)strlen(html), 0);

    closesocket(clientSock);
    closesocket(listenSock);
    WSACleanup();

    size_t codePos = req.find("code=");
    if (codePos == std::string::npos) return "";
    codePos += 5;
    size_t endPos = req.find_first_of("& \r\n", codePos);
    return req.substr(codePos, endPos - codePos);
}

void SaveTokens(const std::string& clientId, const std::string& accessToken, const std::string& refreshToken) {
    std::ofstream out(TOKEN_FILE);
    out << "{\n"
        << "  \"client_id\": \"" << clientId << "\",\n"
        << "  \"access_token\": \"" << accessToken << "\",\n"
        << "  \"refresh_token\": \"" << refreshToken << "\"\n"
        << "}\n";
}

bool LoadTokens(std::string& clientId, std::string& accessToken, std::string& refreshToken) {
    std::ifstream in(TOKEN_FILE);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    clientId = ExtractJsonString(content, "client_id");
    accessToken = ExtractJsonString(content, "access_token");
    refreshToken = ExtractJsonString(content, "refresh_token");
    return !accessToken.empty();
}

bool RefreshAccessToken(const std::string& clientId, std::string& accessToken, std::string& refreshToken) {
    if (refreshToken.empty() || clientId.empty()) return false;
    std::string body = "grant_type=refresh_token"
        "&refresh_token=" + UrlEncode(refreshToken) +
        "&client_id=" + UrlEncode(clientId) +
        "&resource=" + UrlEncode("https://agent.robinhood.com/mcp/trading");

    DWORD status = 0;
    std::string resp = HttpRequest(L"api.robinhood.com", L"/oauth2/token/", L"POST",
        L"Content-Type: application/x-www-form-urlencoded\r\n",
        body, &status);
    if (status == 200) {
        std::string newAccess = ExtractJsonString(resp, "access_token");
        std::string newRefresh = ExtractJsonString(resp, "refresh_token");
        if (!newAccess.empty()) {
            accessToken = newAccess;
            if (!newRefresh.empty()) refreshToken = newRefresh;
            SaveTokens(clientId, accessToken, refreshToken);
            std::cout << "Successfully refreshed Robinhood OAuth token.\n";
            return true;
        }
    }
    return false;
}

bool InteractiveBrowserLogin(std::string& clientId, std::string& accessToken, std::string& refreshToken) {
    std::string regPayload = R"({
        "client_name": "VisualStudioCPPTrader",
        "redirect_uris": [")" + REDIRECT_URI + R"("],
        "grant_types": ["authorization_code", "refresh_token"],
        "response_types": ["code"],
        "token_endpoint_auth_method": "none"
    })";

    DWORD regStatus = 0;
    std::string regResp = HttpRequest(L"agent.robinhood.com", L"/oauth/trading/register", L"POST",
        L"Content-Type: application/json\r\n", regPayload, &regStatus);
    clientId = ExtractJsonString(regResp, "client_id");
    if (clientId.empty()) {
        std::cerr << "Dynamic Client Registration failed (" << regStatus << "): " << regResp << "\n";
        return false;
    }

    std::string verifier, challenge;
    GeneratePkce(verifier, challenge);

    std::string authUrl = "https://robinhood.com/oauth?"
        "response_type=code"
        "&client_id=" + UrlEncode(clientId) +
        "&redirect_uri=" + UrlEncode(REDIRECT_URI) +
        "&code_challenge=" + UrlEncode(challenge) +
        "&code_challenge_method=S256"
        "&scope=internal"
        "&resource=" + UrlEncode("https://agent.robinhood.com/mcp/trading");

    std::cout << "Opening browser for Robinhood authentication...\n";
    ShellExecuteA(nullptr, "open", authUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    std::string code = WaitForOAuthCallbackCode();
    if (code.empty()) {
        std::cerr << "Failed to capture authorization code from redirect.\n";
        return false;
    }

    std::string tokenBody = "grant_type=authorization_code"
        "&code=" + UrlEncode(code) +
        "&redirect_uri=" + UrlEncode(REDIRECT_URI) +
        "&client_id=" + UrlEncode(clientId) +
        "&code_verifier=" + UrlEncode(verifier) +
        "&resource=" + UrlEncode("https://agent.robinhood.com/mcp/trading");

    DWORD tokenStatus = 0;
    std::string tokenResp = HttpRequest(L"api.robinhood.com", L"/oauth2/token/", L"POST",
        L"Content-Type: application/x-www-form-urlencoded\r\n", tokenBody, &tokenStatus);

    accessToken = ExtractJsonString(tokenResp, "access_token");
    refreshToken = ExtractJsonString(tokenResp, "refresh_token");
    if (accessToken.empty()) {
        std::cerr << "Token exchange failed (" << tokenStatus << "): " << tokenResp << "\n";
        return false;
    }

    SaveTokens(clientId, accessToken, refreshToken);
    std::cout << "Tokens saved to " << TOKEN_FILE << "!\n";
    return true;
}

std::string JsonEscape(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\b': o << "\\b";  break;
        case '\f': o << "\\f";  break;
        case '\n': o << "\\n";  break;
        case '\r': o << "\\r";  break;
        case '\t': o << "\\t";  break;
        default:   o << c;      break;
        }
    }
    return o.str();
}

std::string LoadPromptFile(const std::string& filename) {
    std::ifstream in(filename);
    if (!in.is_open()) {
        return "Inspect my Robinhood portfolio and execute autonomous trades if rules permit.";
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string LoadApiKey(const std::string& filename, const char* environmentVariable) {
    std::ifstream in(filename);
    std::string key;
    if (in.is_open()) {
        std::getline(in, key);
        const size_t first = key.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            key.clear();
        }
        else {
            const size_t last = key.find_last_not_of(" \t\r\n");
            key = key.substr(first, last - first + 1);
        }
    }
    return key.empty() ? GetEnvVar(environmentVariable) : key;
}

DWORD CallOpenAITrader(const std::string& openaiKey, const std::string& rhAccessToken) {
    std::string rawPrompt = LoadPromptFile("prompt.txt");
    std::string escapedPrompt = JsonEscape(rawPrompt);

    std::string payload = R"({
        "model": "gpt-6-luna",
        "instructions": "You are running in an unattended single-turn batch script with no human in the loop. The user has pre-authorized all trades and pre-acknowledged all review_option_order and review_equity_order alerts. Whenever you call review_option_order or review_equity_order, you MUST immediately call place_option_order or place_equity_order in the same turn without asking for confirmation.",
        "reasoning": {
            "effort": "medium"
        },
        "tools": [
            {
                "type": "web_search"
            },
            {
                "type": "mcp",
                "server_label": "robinhood_trading",
                "server_url": "https://agent.robinhood.com/mcp/trading",
                "require_approval": "never",
                "headers": {
                    "Authorization": "Bearer )" + rhAccessToken + R"("
                }
            }
        ],
        "input": ")" + escapedPrompt + R"("
    })";

    std::wstring headers = L"Content-Type: application/json\r\n"
        L"Authorization: Bearer " + ToWide(openaiKey) + L"\r\n";

    DWORD status = 0;
    std::cout << "Running OpenAI Agent with Robinhood MCP (gpt-6-luna)...\n"
        << "Waiting for the provider and MCP tools to finish...\n" << std::flush;
    std::string resp = HttpRequestWithProgress(L"api.openai.com", L"/v1/responses", L"POST",
        headers, payload, &status);

    PrintStatusSummary(resp, status);

    return status;
}

DWORD CallClaudeTrader(const std::string& claudeKey, const std::string& rhAccessToken) {
    std::string prompt = JsonEscape(LoadPromptFile("prompt.txt"));
    std::string token = JsonEscape(rhAccessToken);

    std::string payload = R"({
        "model": "claude-sonnet-4-5",
        "max_tokens": 8192,
        "system": "You are running in an unattended single-turn batch script with no human in the loop. The user has pre-authorized all trades and pre-acknowledged all review_option_order and review_equity_order alerts. Whenever you call review_option_order or review_equity_order, you MUST immediately call place_option_order or place_equity_order in the same turn without asking for confirmation.",
        "mcp_servers": [
            {
                "type": "url",
                "url": "https://agent.robinhood.com/mcp/trading",
                "name": "robinhood_trading",
                "authorization_token": ")" + token + R"("
            }
        ],
        "tools": [
            {
                "type": "web_search_20250305",
                "name": "web_search",
                "max_uses": 5
            },
            {
                "type": "mcp_toolset",
                "mcp_server_name": "robinhood_trading"
            }
        ],
        "messages": [
            {
                "role": "user",
                "content": ")" + prompt + R"("
            }
        ]
    })";

    std::wstring headers = L"Content-Type: application/json\r\n"
        L"x-api-key: " + ToWide(claudeKey) + L"\r\n"
        L"anthropic-version: 2023-06-01\r\n"
        L"anthropic-beta: mcp-client-2025-11-20\r\n";

    DWORD status = 0;
    std::cout << "Running Claude with Robinhood MCP (claude-sonnet-4-5)...\n"
        << "Waiting for the provider and MCP tools to finish...\n" << std::flush;
    std::string response = HttpRequestWithProgress(L"api.anthropic.com", L"/v1/messages", L"POST",
        headers, payload, &status);
    PrintStatusSummary(response, status);
    return status;
}
int main(int argc, char* argv[]) {
    std::string provider = "openai";
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--provider") {
            if (i + 1 >= argc) {
                std::cerr << "Usage: RobinhoodAutomation.exe [--provider openai|claude]\n";
                return 1;
            }
            provider = argv[++i];
        }
        else if (argument.rfind("--provider=", 0) == 0) {
            provider = argument.substr(std::string("--provider=").size());
        }
        else {
            std::cerr << "Unknown argument: " << argument
                << "\nUsage: RobinhoodAutomation.exe [--provider openai|claude]\n";
            return 1;
        }
    }



    if (provider != "openai" && provider != "claude") {
        std::cerr << "Invalid provider '" << provider
            << "'. Choose openai or claude.\n";
        return 1;
    }

    const bool useClaude = provider == "claude";
    const std::string apiKey = useClaude
        ? LoadApiKey("claude_key.txt", "ANTHROPIC_API_KEY")
        : LoadApiKey("openai_key.txt", "OPENAI_API_KEY");
    if (apiKey.empty()) {
        std::cerr << (useClaude
            ? "Error: Add your API key to claude_key.txt or set ANTHROPIC_API_KEY.\n"
            : "Error: Add your API key to openai_key.txt or set OPENAI_API_KEY.\n");
        return 1;
    }

    std::string clientId, accessToken, refreshToken;

    if (!LoadTokens(clientId, accessToken, refreshToken)) {
        if (!InteractiveBrowserLogin(clientId, accessToken, refreshToken)) {
            return 1;
        }
    }

    DWORD status = useClaude
        ? CallClaudeTrader(apiKey, accessToken)
        : CallOpenAITrader(apiKey, accessToken);
    if (status == 401) {
        std::cout << "Attempting automatic OAuth token refresh...\n";
        if (RefreshAccessToken(clientId, accessToken, refreshToken)) {
            if (useClaude) {
                CallClaudeTrader(apiKey, accessToken);
            }
            else {
                CallOpenAITrader(apiKey, accessToken);
            }
        }
    }
    else if (status != 200) {
        std::cerr << "The request was not retried automatically because its outcome may be unknown. "
            << "Check Robinhood order activity before starting another run.\n";
    }

    return 0;
}