// Quality-list tests for NdmMsgHost::MediaReplyFromJson (yt-dlp -J -> rows).
//   mediatest                 synthetic fixtures (asserts)
//   mediatest <file.json>...  prints the rows built from real yt-dlp dumps
#include "../../src/msghost/ndm_msghost.h"
#include "../../src/common/ndm_common.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0, g_n = 0;

static void Check(bool ok, const char* what)
{
    ++g_n;
    if (!ok) { ++g_fail; printf("FAIL: %s\n", what); }
}

// "label|format|ext|filesize|approx" per row, in order.
static std::vector<std::string> Rows(const std::string& reply)
{
    std::vector<std::string> out;
    auto field = [](const std::string& o, const char* key) {
        std::string k = std::string("\"") + key + "\":";
        size_t p = o.find(k);
        if (p == std::string::npos) return std::string();
        p += k.size();
        if (o[p] == '"') { size_t e = o.find('"', p + 1); return o.substr(p + 1, e - p - 1); }
        size_t e = o.find_first_of(",}", p);
        return o.substr(p, e - p);
    };
    size_t at = reply.find("\"formats\":[");
    while (at != std::string::npos) {
        size_t b = reply.find("{\"label\"", at);
        if (b == std::string::npos) break;
        size_t e = reply.find('}', b);
        std::string o = reply.substr(b, e - b + 1);
        out.push_back(field(o, "label") + "|" + field(o, "format") + "|" + field(o, "ext") + "|" +
                      field(o, "filesize") + "|" + field(o, "approx"));
        at = e;
    }
    return out;
}

static std::vector<std::string> Audio(const std::string& json)
{
    std::vector<std::string> a;
    for (const std::string& r : Rows(NdmMsgHost::MediaReplyFromJson(json)))
        if (r.find("Audio") != std::string::npos || r.compare(0, 4, "MP3 ") == 0) a.push_back(r);
    return a;
}

static void Expect(const std::string& json, const std::vector<std::string>& want, const char* what)
{
    std::vector<std::string> got = Audio(json);
    bool ok = got == want;
    Check(ok, what);
    if (!ok) {
        printf("  got:\n");
        for (auto& g : got) printf("    %s\n", g.c_str());
        printf("  want:\n");
        for (auto& w : want) printf("    %s\n", w.c_str());
    }
}

static const char* kVideo =
    "{\"format_id\":\"137\",\"ext\":\"mp4\",\"vcodec\":\"avc1.640028\",\"acodec\":\"none\",\"height\":1080,\"tbr\":4000,\"filesize\":50000000}";

static std::string Doc(const std::string& fmts)
{
    return "{\"title\":\"T\",\"duration\":200,\"extractor_key\":\"Youtube\",\"formats\":[" + fmts + "]}";
}

int main(int argc, char** argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::ifstream f(argv[i], std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            std::string j = ss.str();
            if (j.size() >= 3 && (unsigned char)j[0] == 0xEF) j = j.substr(3);
            printf("== %s\n", argv[i]);
            for (auto& r : Rows(NdmMsgHost::MediaReplyFromJson(j))) printf("  %s\n", r.c_str());
        }
        return 0;
    }

    // Opus 160 beats AAC 128 (160 x 1.3 = 208 > 128); MP3 from the 160 source: 160 and 128.
    Expect(Doc(std::string(kVideo) +
        ",{\"format_id\":\"251\",\"ext\":\"webm\",\"vcodec\":\"none\",\"acodec\":\"opus\",\"abr\":160.2,\"filesize\":4000000}"
        ",{\"format_id\":\"140\",\"ext\":\"m4a\",\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\",\"abr\":128,\"filesize_approx\":3200000}"
        ",{\"format_id\":\"249\",\"ext\":\"webm\",\"vcodec\":\"none\",\"acodec\":\"opus\",\"abr\":50}"),
        { "Best Audio (Opus 160 kbps)|251|webm|4000000|false",
          "Audio M4A 128 kbps|140|m4a|3200000|true",
          "Audio Opus 50 kbps|249|webm|0|false",
          "MP3 160 kbps (converted)|x:mp3:160|251/ba/b|mp3|0|false",
          "MP3 128 kbps (converted)|x:mp3:128|251/ba/b|mp3|0|false" },
        "Opus 160 > AAC 128; exact ids; sizes only from yt-dlp; MP3 <= source");

    // AAC 256 beats Opus 160 (256 > 208); a 256 kbps source may offer MP3 320.
    Expect(Doc(std::string(kVideo) +
        ",{\"format_id\":\"251\",\"ext\":\"webm\",\"vcodec\":\"none\",\"acodec\":\"opus\",\"abr\":160}"
        ",{\"format_id\":\"141\",\"ext\":\"m4a\",\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\",\"abr\":256}"),
        { "Best Audio (M4A 256 kbps)|141|m4a|0|false",
          "Audio Opus 160 kbps|251|webm|0|false",
          "MP3 320 kbps (converted)|x:mp3:320|141/ba/b|mp3|0|false",
          "MP3 256 kbps (converted)|x:mp3:256|141/ba/b|mp3|0|false",
          "MP3 128 kbps (converted)|x:mp3:128|141/ba/b|mp3|0|false" },
        "AAC 256 > Opus 160; 320 only for >= 256");

    // X: HLS audio renditions, no codec in the metadata, AAC by address; MP4 container -> copied into .m4a.
    Expect("{\"title\":\"x\",\"duration\":30,\"formats\":["
        "{\"format_id\":\"hls-audio-32000-Audio\",\"ext\":\"mp4\",\"vcodec\":\"none\",\"abr\":32,\"tbr\":32,\"url\":\"https://video.twimg.com/amplify_video/1/pl/mp4a/32000/a.m3u8\"},"
        "{\"format_id\":\"hls-audio-128000-Audio\",\"ext\":\"mp4\",\"vcodec\":\"none\",\"abr\":128,\"tbr\":128,\"url\":\"https://video.twimg.com/amplify_video/1/pl/mp4a/128000/b.m3u8\"},"
        "{\"format_id\":\"hls-audio-64000-Audio\",\"ext\":\"mp4\",\"vcodec\":\"none\",\"abr\":64,\"tbr\":64,\"url\":\"https://video.twimg.com/amplify_video/1/pl/mp4a/64000/c.m3u8\"},"
        "{\"format_id\":\"hls-2176\",\"ext\":\"mp4\",\"vcodec\":\"avc1.640020\",\"acodec\":\"none\",\"height\":720,\"tbr\":2176}]}",
        { "Best Audio (M4A 128 kbps)|x:m4a|hls-audio-128000-Audio|m4a|0|false",
          "Audio M4A 64 kbps|x:m4a|hls-audio-64000-Audio|m4a|0|false",
          "Audio M4A 32 kbps|x:m4a|hls-audio-32000-Audio|m4a|0|false",
          "MP3 128 kbps (converted)|x:mp3:128|hls-audio-128000-Audio/ba/b|mp3|0|false" },
        "X renditions: AAC by address, never a size from tbr x duration, MP3 128 only");

    // Original language only; DRC and nameless HLS copies dropped; duplicates collapsed.
    Expect(Doc(std::string(kVideo) +
        ",{\"format_id\":\"251-0\",\"ext\":\"webm\",\"vcodec\":\"none\",\"acodec\":\"opus\",\"abr\":130,\"language\":\"de\",\"language_preference\":-1}"
        ",{\"format_id\":\"251-1\",\"ext\":\"webm\",\"vcodec\":\"none\",\"acodec\":\"opus\",\"abr\":120,\"language\":\"en\",\"language_preference\":10}"
        ",{\"format_id\":\"251-drc\",\"ext\":\"webm\",\"vcodec\":\"none\",\"acodec\":\"opus\",\"abr\":125,\"language_preference\":10}"
        ",{\"format_id\":\"250-1\",\"ext\":\"webm\",\"vcodec\":\"none\",\"acodec\":\"opus\",\"abr\":120.3,\"language_preference\":10}"
        ",{\"format_id\":\"234\",\"ext\":\"mp4\",\"vcodec\":\"none\",\"language_preference\":10}"),
        { "Best Audio (Opus 120 kbps)|250-1|webm|0|false",
          "MP3 96 kbps (converted)|x:mp3:96|250-1/ba/b|mp3|0|false" },
        "original track only, no DRC / unknown HLS copy, one row per codec+bitrate");

    // A single audio track: one Best Audio row plus the honest MP3 options.
    Expect(Doc(std::string(kVideo) +
        ",{\"format_id\":\"140\",\"ext\":\"m4a\",\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\",\"abr\":129.8,\"filesize\":3000000}"),
        { "Best Audio (M4A 130 kbps)|140|m4a|3000000|false",
          "MP3 128 kbps (converted)|x:mp3:128|140/ba/b|mp3|0|false" },
        "single track: Best Audio + MP3 128 (129.8 counts as 128)");

    // Muxed only (no separate audio): extract the video's own AAC track.
    Expect(Doc("{\"format_id\":\"18\",\"ext\":\"mp4\",\"vcodec\":\"avc1.42001E\",\"acodec\":\"mp4a.40.2\",\"height\":360,\"abr\":96,\"tbr\":500}"),
        { "Audio M4A|x:m4a|ba/b|m4a|0|false",
          "MP3 96 kbps (converted)|x:mp3:96|ba/b|mp3|0|false" },
        "muxed only: AAC copied out, MP3 at the track's bitrate");

    // Video rows unchanged: one Best first, merged with audio.
    {
        std::vector<std::string> r = Rows(NdmMsgHost::MediaReplyFromJson(Doc(std::string(kVideo) +
            ",{\"format_id\":\"136\",\"ext\":\"mp4\",\"vcodec\":\"avc1\",\"acodec\":\"none\",\"height\":720,\"tbr\":2000}"
            ",{\"format_id\":\"140\",\"ext\":\"m4a\",\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\",\"abr\":128}")));
        Check(r.size() >= 2 && r[0].compare(0, 17, "Best (1080p MP4)|") == 0 && r[1].compare(0, 9, "720p MP4|") == 0,
              "video: Best first, then the other resolutions");
        int best = 0;
        for (auto& x : r) if (x.compare(0, 6, "Best (") == 0) ++best;
        Check(best == 1, "exactly one video Best row");
    }

    // E. explicit "Download with NDM" sends are never flagged as automatic
    //    captures (the main window drops only autoCapture ones when capture is
    //    off). source "capture" is not sent here: that path reads NDM's
    //    BrowserMonitor registry mirror.
    {
        HWND w = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        NdmMsgHost::SetMainWindow(w);
        for (const char* src : { "", "video_button", "menu_link" }) {
            std::string req = std::string("{\"id\":1,\"action\":\"addDownload\",\"url\":\"https://ex.com/a.zip\"") +
                              (*src ? std::string(",\"source\":\"") + src + "\"" : std::string()) + "}";
            std::string r = NdmMsgHost::HandleMessage(req);
            MSG m{};
            NdmBrowserRequest* br = nullptr;
            while (PeekMessageW(&m, w, 0, 0, PM_REMOVE))
                if (m.message == WM_NDM_BROWSER_MSG) br = NdmTakeBrowserRequest(m.lParam);
            Check(r.find("accepted") != std::string::npos && br && !br->autoCapture, "explicit send accepted, not an auto capture");
            delete br;
        }
        NdmMsgHost::SetMainWindow(nullptr);
        DestroyWindow(w);
    }

    printf("%s: %d/%d checks\n", g_fail ? "FAILED" : "all media list tests passed", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
