// Loopback playlist server of ndm_media.cpp (pair| downloads):
//   plsrvtest.exe
// The .cpp is compiled into this test (unity include) so its file-local
// PlaylistServer / JoinedPlaylistBody / LoopbackProxyEnv can be driven
// directly: exactly one path, GET only, loopback only, gone after Stop().
#include "../../src/media/ndm_media.cpp"

#include <cstdio>

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) { ++g_pass; } else { ++g_fail; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static USHORT PortOf(const std::wstring& url)
{
    size_t c = url.find(L':', 7);
    return c == std::wstring::npos ? 0 : (USHORT)_wtoi(url.c_str() + c + 1);
}

// Raw request -> full response ("" if the connection failed).
static std::string Ask(USHORT port, const std::string& req, bool* connected = nullptr)
{
    if (connected) *connected = false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        if (connected) *connected = true;
        DWORD to = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        send(s, req.data(), (int)req.size(), 0);
        char buf[4096];
        int n;
        while ((n = recv(s, buf, sizeof(buf), 0)) > 0) out.append(buf, (size_t)n);
    }
    closesocket(s);
    return out;
}

int main()
{
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);

    // Body
    const std::wstring v = L"https://video.twimg.com/amplify_video/1/pl/avc1/320x568/a.m3u8";
    const std::wstring au = L"https://video.twimg.com/amplify_video/1/pl/mp4a/128000/b.m3u8";
    std::string body = JoinedPlaylistBody(v, au);
    CHECK(body.find("#EXTM3U\n") == 0, "body starts with #EXTM3U");
    CHECK(body.find("URI=\"https://video.twimg.com/amplify_video/1/pl/mp4a/128000/b.m3u8\"") != std::string::npos,
          "audio rendition");
    CHECK(body.find("CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=320x568,AUDIO=\"aud\"\n"
                    "https://video.twimg.com/amplify_video/1/pl/avc1/320x568/a.m3u8\n") != std::string::npos,
          "video variant with codecs + resolution");
    CHECK(body.find("file:") == std::string::npos, "no file URL anywhere");

    const std::wstring token = RandomTag(16);
    CHECK(token.size() == 32, "token has 32 hex characters");
    const std::string tok8 = WideToUtf8(token);
    PlaylistServer srv(body, token);
    CHECK(srv.Start(), "server starts");
    std::wstring url = srv.Url();
    USHORT port = PortOf(url);
    CHECK(url == L"http://127.0.0.1:" + std::to_wstring(port) + L"/" + token + L".m3u8" && port > 0,
          "URL %ls", url.c_str());

    std::string ok = Ask(port, "GET /" + tok8 + ".m3u8 HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: */*\r\n\r\n");
    CHECK(ok.compare(0, 15, "HTTP/1.1 200 OK") == 0, "GET token path -> 200 (%s)", ok.substr(0, 40).c_str());
    CHECK(ok.find("\r\nContent-Type: application/vnd.apple.mpegurl\r\n") != std::string::npos, "HLS content type");
    CHECK(ok.find("\r\nContent-Length: " + std::to_string(body.size()) + "\r\n") != std::string::npos, "content length");
    CHECK(ok.size() > body.size() && ok.compare(ok.size() - body.size(), body.size(), body) == 0, "body served");
    std::string again = Ask(port, "GET /" + tok8 + ".m3u8 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(again.compare(0, 15, "HTTP/1.1 200 OK") == 0, "served more than once (yt-dlp fetches it twice)");

    struct { std::string req; const char* want; const char* what; } cases[] = {
        { "GET / HTTP/1.1\r\n\r\n", "HTTP/1.1 404", "root" },
        { "GET /" + tok8 + ".m3u8?x=1 HTTP/1.1\r\n\r\n", "HTTP/1.1 404", "query string" },
        { "GET /" + tok8.substr(0, 31) + ".m3u8 HTTP/1.1\r\n\r\n", "HTTP/1.1 404", "wrong token" },
        { "GET /" + tok8 + ".m3u8/../x HTTP/1.1\r\n\r\n", "HTTP/1.1 404", "path tricks" },
        { "GET /C:/Windows/win.ini HTTP/1.1\r\n\r\n", "HTTP/1.1 404", "file-like path" },
        { "POST /" + tok8 + ".m3u8 HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 405", "POST" },
        { "HEAD /" + tok8 + ".m3u8 HTTP/1.1\r\n\r\n", "HTTP/1.1 405", "HEAD" },
        { "get /" + tok8 + ".m3u8 HTTP/1.1\r\n\r\n", "HTTP/1.1 405", "lower-case method" },
        { "GET /" + tok8 + ".m3u8\r\n\r\n", "HTTP/1.1 400", "HTTP/0.9 style" },
        { "garbage\r\n\r\n", "HTTP/1.1 400", "garbage" },
    };
    for (const auto& c : cases) {
        std::string r = Ask(port, c.req);
        CHECK(r.compare(0, strlen(c.want), c.want) == 0, "%s -> %s (got %s)", c.what, c.want, r.substr(0, 30).c_str());
        CHECK(r.find("mpegurl") == std::string::npos && r.find("#EXTM3U") == std::string::npos, "%s: no playlist", c.what);
    }
    {   // incomplete request: closed after the timeout, without the playlist
        DWORD t0 = GetTickCount();
        std::string r = Ask(port, "GET /" + tok8 + ".m3u8 HTTP/1.1\r\n");
        CHECK(r.find("#EXTM3U") == std::string::npos && GetTickCount() - t0 < 6000, "incomplete request not served");
    }

    srv.Stop();
    bool connected = true;
    Ask(port, "GET /" + tok8 + ".m3u8 HTTP/1.1\r\n\r\n", &connected);
    CHECK(!connected, "nothing listens after Stop()");
    srv.Stop();   // idempotent

    // Proxy exemption for the loopback address
    SetEnvironmentVariableW(L"HTTPS_PROXY", L"http://proxy.example:3128");
    SetEnvironmentVariableW(L"NO_PROXY", L"intranet.example");
    std::vector<std::wstring> env = LoopbackProxyEnv();
    CHECK(env.size() == 1 && env[0] == L"NO_PROXY=intranet.example,127.0.0.1", "env proxy: NO_PROXY extended (%zu)",
          env.size());
    std::vector<wchar_t> block = BuildEnv(env);
    int noProxyCount = 0;
    bool extended = false;
    for (const wchar_t* p = block.data(); *p; p += wcslen(p) + 1)
        if (_wcsnicmp(p, L"NO_PROXY=", 9) == 0) { ++noProxyCount; extended = wcscmp(p + 9, L"intranet.example,127.0.0.1") == 0; }
    CHECK(noProxyCount == 1 && extended, "child environment has exactly one, extended NO_PROXY");
    SetEnvironmentVariableW(L"HTTPS_PROXY", nullptr);
    SetEnvironmentVariableW(L"NO_PROXY", nullptr);

    printf("plsrvtest: %d passed, %d failed\n", g_pass, g_fail);
    WSACleanup();
    return g_fail ? 1 : 0;
}
