// extract_subs.cpp
// Compile with: g++ -std=c++17 -O2 -o extract_subs extract_subs.cpp `pkg-config --cflags --libs libavformat libavcodec libavutil`

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <locale>
#include <regex>
#include <cctype>
#include <sstream>
#include <map>

#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/avutil.h>
}

using std::string;
using std::vector;


#ifdef _WIN32
#include <windows.h>

// normalize a locale-like string into a two-letter lowercase lang (e.g., "ru-RU" -> "ru")
static std::string normalize_lang(const std::string& s) {
    if (s.empty()) return std::string();
    size_t i = 0;
    while (i < s.size() && !std::isalpha((unsigned char)s[i])) ++i;
    size_t j = i;
    while (j < s.size() && std::isalpha((unsigned char)s[j])) ++j;
    std::string pref = s.substr(i, j - i);
    std::transform(pref.begin(), pref.end(), pref.begin(), [](unsigned char c) { return std::tolower(c); });
    if (pref.size() >= 2) pref = pref.substr(0, 2);
    return pref;
}

static std::string get_system_language() {
    // 1) Preferred API: GetUserDefaultLocaleName (Vista+)
    WCHAR localeName[LOCALE_NAME_MAX_LENGTH] = { 0 };
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        char buf[LOCALE_NAME_MAX_LENGTH] = { 0 };
        int n = WideCharToMultiByte(CP_UTF8, 0, localeName, -1, buf, (int)sizeof(buf), nullptr, nullptr);
        if (n > 0) return normalize_lang(std::string(buf));
    }

    // 2) Fallback: GetUserDefaultLangID + GetLocaleInfoA
    LANGID lid = GetUserDefaultLangID();
    if (lid != 0) {
        LCID lcid = MAKELCID(lid, SORT_DEFAULT);
        char buf[16] = { 0 };
        if (GetLocaleInfoA(lcid, LOCALE_SISO639LANGNAME, buf, (int)sizeof(buf)) > 0) {
            return normalize_lang(std::string(buf));
        }
    }

    // 3) Fallback to LANG environment variable (safe)
    std::string langEnv = std::getenv("LANG");
    if (!langEnv.empty()) return normalize_lang(langEnv);

    // 4) Fallback to C++ locale
    try {
        std::locale loc("");
        std::string name = loc.name(); // examples: "Russian_Russia.1251", "ru_RU"
        if (!name.empty()) return normalize_lang(name);
    }
    catch (...) {}

    // final fallback
    return "en";
}

#else // POSIX / other


static std::string normalize_lang(const std::string& s) {
    if (s.empty()) return std::string();
    size_t i = 0;
    while (i < s.size() && !std::isalpha((unsigned char)s[i])) ++i;
    size_t j = i;
    while (j < s.size() && std::isalpha((unsigned char)s[j])) ++j;
    std::string pref = s.substr(i, j - i);
    std::transform(pref.begin(), pref.end(), pref.begin(), [](unsigned char c) { return std::tolower(c); });
    if (pref.size() >= 2) pref = pref.substr(0, 2);
    return pref;
}

static std::string get_system_language() {
    std::string langEnv = std::getenv("LANG");
    if (!langEnv.empty()) return normalize_lang(langEnv);
    try {
        std::locale loc("");
        std::string name = loc.name();
        if (!name.empty()) return normalize_lang(name);
    }
    catch (...) {}
    return "en";
}

#endif
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static string html_unescape(const string& s) {
    string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 4; continue; }
            if (s.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 3; continue; }
            if (s.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 3; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 5; continue; }
            if (s.compare(i, 5, "&#39;") == 0) { out.push_back('\''); i += 4; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

/*
 *  from mpv/sub/sd_ass.c
 * ass_to_plaintext() was written by wm4 and he says it can be under LGPL
 */
std::string ass_to_plaintext(const char* in)
{
    std::string result;

    bool in_tag = false;
    const char* open_tag_pos = nullptr;
    bool in_drawing = false;
    while (*in) {
        if (in_tag) {
            if (in[0] == '}') {
                in += 1;
                in_tag = false;
            }
            else if (in[0] == '\\' && in[1] == 'p') {
                in += 2;
                // Skip text between \pN and \p0 tags. A \p without a number
                // is the same as \p0, and leading 0s are also allowed.
                in_drawing = false;
                while (in[0] >= '0' && in[0] <= '9') {
                    if (in[0] != '0')
                        in_drawing = true;
                    in += 1;
                }
            }
            else {
                in += 1;
            }
        }
        else {
            if (in[0] == '\\' && (in[1] == 'N' || in[1] == 'n')) {
                in += 2;
                result += '\n';
            }
            else if (in[0] == '\\' && in[1] == 'h') {
                in += 2;
                result += ' ';
            }
            else if (in[0] == '{') {
                open_tag_pos = in;
                in += 1;
                in_tag = true;
            }
            else {
                if (!in_drawing)
                    result += in[0];
                in += 1;
            }
        }
    }
    // A '{' without a closing '}' is always visible.
    if (in_tag) {
        result += open_tag_pos;
    }

    return result;
}

std::string fromAss(const char* ass) {
    auto b = ass_to_plaintext(ass);
    int hour1, min1, sec1, hunsec1, hour2, min2, sec2, hunsec2;
    char line[1024];
    // fixme: "\0" maybe not allowed
    if (sscanf(b.c_str(), "Dialogue: Marked=%*d,%d:%d:%d.%d,%d:%d:%d.%d%1023[^\r\n]", //&nothing,
        &hour1, &min1, &sec1, &hunsec1,
        &hour2, &min2, &sec2, &hunsec2,
        line) < 9)
        if (sscanf(b.c_str(), "Dialogue: %*d,%d:%d:%d.%d,%d:%d:%d.%d%1023[^\r\n]", //&nothing,
            &hour1, &min1, &sec1, &hunsec1,
            &hour2, &min2, &sec2, &hunsec2,
            line) < 9)
            if (sscanf(b.c_str(), "%d,%d%1023[^\r\n]",  //&nothing,
                &sec1, &sec2, line) < 3)
                return b;  // libass ASS_Event.Text has no Dialogue
    auto ret = strchr(line, ',');
    if (!ret)
        return line;
    static const char kDefaultStyle[] = "Default,";
    for (int comma = 0; comma < 6; comma++) {
        if (!(ret = strchr(++ret, ','))) {
            // workaround for ffmpeg decoded srt in ass format: "Dialogue: 0,0:42:29.20,0:42:31.08,Default,Chinese\NEnglish.
            if (!(ret = strstr(line, kDefaultStyle))) {
                if (line[0] == ',') //work around for libav-9-
                    return line + 1;
                return line;
            }
            ret += sizeof(kDefaultStyle) - 1 - 1; // tail \0
        }
    }
    ret++;
    const auto p = strcspn(b.c_str(), "\r\n");
    if (p == b.size()) //not found
        return ret;

    std::string line2 = b.substr(p + 1);
    line2 = trim(line2);
    if (line2.empty())
        return ret;

    return ret + ("\n" + line2);
}

static string sanitize_sub_text(const string& in) {
    string s = in;
    // 1) Remove ASS/SSA override blocks like {\...}
    s = std::regex_replace(s, std::regex(R"(\\N)"), "\n"); // explicit \N -> newline
    s = std::regex_replace(s, std::regex(R"(\{[^}]*\})"), ""); // {...}
    // 2) Remove any HTML/XML tags <...>
    s = std::regex_replace(s, std::regex(R"(<[^>]*>)"), "");
    // 3) Unescape a few HTML entities
    s = html_unescape(s);
    // 4) Replace multiple spaces with single, and tidy newlines
    // Normalize CRLF to LF
    s = std::regex_replace(s, std::regex("\r\n"), "\n");
    s = std::regex_replace(s, std::regex("\r"), "\n");
    // Collapse multiple spaces
    s = std::regex_replace(s, std::regex("[ \t]{2,}"), " ");
    // Trim each line and drop leading/trailing blank lines
    std::istringstream iss(s);
    string line, out;
    bool firstLine = true;
    while (std::getline(iss, line)) {
        string t = trim(line);
        if (t.empty()) {
            out += "\n";
        }
        else {
            out += t;
            out += "\n";
        }
    }
    // Collapse multiple blank lines to a single blank line
    out = std::regex_replace(out, std::regex("\n{3,}"), "\n\n");
    // Trim outer whitespace
    out = trim(out);
    // Replace any occurrence of multiple internal newlines with one where appropriate
    out = std::regex_replace(out, std::regex("\n{2,}"), "\n\n");
    return out;
}

struct Cue {
    int64_t start_ms;
    int64_t end_ms;
    string text;
};

static string fmt_srt_time(int64_t ms) {
    int64_t total_ms = ms;
    int64_t hours = total_ms / 3600000;
    total_ms %= 3600000;
    int64_t minutes = total_ms / 60000;
    total_ms %= 60000;
    int64_t seconds = total_ms / 1000;
    int64_t millis = total_ms % 1000;
    char buf[64];
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
        (long long)hours, (long long)minutes, (long long)seconds, (long long)millis);
    return string(buf);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: extract_subs <video-file>\n";
        return 1;
    }
    string infile = argv[1];

    av_log_set_level(AV_LOG_ERROR);
    //av_register_all();

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, infile.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Error: cannot open file: " << infile << "\n";
        return 2;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::cerr << "Error: cannot find stream info\n";
        avformat_close_input(&fmt);
        return 3;
    }

    string syslang = get_system_language();
    std::transform(syslang.begin(), syslang.end(), syslang.begin(), ::tolower);
    if (syslang.empty()) syslang = "en";

    // Find subtitle streams that match system language or have no language but chosen as fallback
    vector<int> candidate_streams;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        AVDictionaryEntry* tag = av_dict_get(st->metadata, "language", nullptr, 0);
        string lang = tag ? string(tag->value) : "";
        std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);
        if (!lang.empty()) {
            // accept if starts with syslang or equals
            if (lang == syslang || lang.find(syslang) == 0) {
                candidate_streams.push_back((int)i);
            }
            else {
                // also accept 2-letter prefix matches (e.g., "en" vs "eng")
                if (lang.size() >= 2 && syslang.size() >= 2 && lang.substr(0, 2) == syslang.substr(0, 2)) {
                    candidate_streams.push_back((int)i);
                }
            }
        }
        else {
            // no language tag -> keep as fallback but later only use if we have no explicit matches
            candidate_streams.push_back((int)i);
        }
    }

    if (candidate_streams.empty()) {
        std::cerr << "No subtitle streams found for language '" << syslang << "'. Trying all subtitle streams.\n";
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                candidate_streams.push_back((int)i);
    }

    if (candidate_streams.empty()) {
        std::cerr << "No subtitle streams in file.\n";
        avformat_close_input(&fmt);
        return 4;
    }

    // We'll decode each candidate stream and collect cues
    vector<Cue> cues;

    for (int si : candidate_streams) {
        AVStream* st = fmt->streams[si];
        // open codec context
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            std::cerr << "Warning: no decoder for stream " << si << "\n";
            continue;
        }
        AVCodecContext* cctx = avcodec_alloc_context3(dec);
        if (!cctx) continue;
        if (avcodec_parameters_to_context(cctx, st->codecpar) < 0) {
            avcodec_free_context(&cctx);
            continue;
        }
        if (avcodec_open2(cctx, dec, nullptr) < 0) {
            avcodec_free_context(&cctx);
            continue;
        }

        AVPacket pkt{};
        //av_init_packet(&pkt);
        pkt.data = nullptr; pkt.size = 0;

        // Seek to beginning
        av_seek_frame(fmt, si, 0, AVSEEK_FLAG_BACKWARD);

        // Read packets
        while (av_read_frame(fmt, &pkt) >= 0) {
            if (pkt.stream_index != si) {
                av_packet_unref(&pkt);
                continue;
            }
            // decode subtitle packet
            AVSubtitle sub;
            int got_sub = 0;
            int ret = avcodec_decode_subtitle2(cctx, &sub, &got_sub, &pkt);
            if (ret < 0) {
                av_packet_unref(&pkt);
                continue;
            }
            if (got_sub) {
                string accumulated;
                for (unsigned i = 0; i < sub.num_rects; ++i) {
                    AVSubtitleRect* rect = sub.rects[i];
                    if (!rect) continue;
                    string text;
                    if (rect->ass && strlen(rect->ass) > 0) {
                        // prefer ASS if available
                        text = fromAss(rect->ass);
                    }
                    else {
                        if (!!rect->text)
                            continue;
                        text = rect->text;
                    }
                    if (!accumulated.empty()) accumulated += "\n";
                    accumulated += text;
                }
                // convert pts/duration to milliseconds
                int64_t start_ms = AV_NOPTS_VALUE;
                int64_t end_ms = AV_NOPTS_VALUE;
                if (sub.pts != AV_NOPTS_VALUE) {
                    // subtitle pts is in AV_TIME_BASE units? In many builds it's time in AV_TIME_BASE units.
                    // Try to convert using stream time_base if available
                    AVRational tb = st->time_base;
                    if (tb.num && tb.den)
                        start_ms = av_rescale_q(sub.pts, tb, AVRational{ 1,1000 });
                    else
                        start_ms = av_rescale_q(sub.pts, AVRational{ 1, AV_TIME_BASE }, AVRational{ 1,1000 });
                }
                if (sub.end_display_time > 0) {
                    // end_display_time is in milliseconds relative to start (legacy)
                    if (start_ms != AV_NOPTS_VALUE)
                        end_ms = start_ms + sub.end_display_time;
                    else
                        end_ms = sub.end_display_time;
                }
                else if (sub.pts != AV_NOPTS_VALUE && sub.rects && sub.num_rects > 0) {
                    // fallback: try pkt.pts / stream time base and pkt.duration
                    if (pkt.pts != AV_NOPTS_VALUE) {
                        AVRational tb = st->time_base;
                        int64_t pkt_ms = (tb.num && tb.den) ? av_rescale_q(pkt.pts, tb, AVRational{ 1,1000 }) : av_rescale_q(pkt.pts, AVRational{ 1,AV_TIME_BASE }, AVRational{ 1,1000 });
                        start_ms = pkt_ms;
                        if (pkt.duration > 0) {
                            int64_t dur_ms = (tb.num && tb.den) ? av_rescale_q(pkt.duration, tb, AVRational{ 1,1000 }) : 0;
                            end_ms = pkt_ms + dur_ms;
                        }
                        else {
                            end_ms = start_ms + 2000; // default display 2s
                        }
                    }
                    else {
                        start_ms = 0;
                        end_ms = start_ms + 2000;
                    }
                }
                else {
                    // last resort: use packet DTS/PTS
                    if (pkt.pts != AV_NOPTS_VALUE) {
                        AVRational tb = st->time_base;
                        start_ms = (tb.num && tb.den) ? av_rescale_q(pkt.pts, tb, AVRational{ 1,1000 }) : 0;
                        if (pkt.duration > 0)
                            end_ms = start_ms + ((tb.num && tb.den) ? av_rescale_q(pkt.duration, tb, AVRational{ 1,1000 }) : 2000);
                        else
                            end_ms = start_ms + 2000;
                    }
                    else {
                        start_ms = 0;
                        end_ms = 2000;
                    }
                }
                if (start_ms == AV_NOPTS_VALUE) start_ms = 0;
                if (end_ms == AV_NOPTS_VALUE || end_ms < start_ms) end_ms = start_ms + 2000;

                string clean = sanitize_sub_text(accumulated);
                if (!clean.empty()) {
                    Cue c;
                    c.start_ms = start_ms;
                    c.end_ms = end_ms;
                    c.text = clean;
                    cues.push_back(c);
                }
                avsubtitle_free(&sub);
            }
            av_packet_unref(&pkt);
        }

        avcodec_close(cctx);
        avcodec_free_context(&cctx);
        // rewind format context for next stream processing
        avformat_seek_file(fmt, -1, INT64_MIN, 0, INT64_MAX, 0);
    }

    avformat_close_input(&fmt);

    if (cues.empty()) {
        std::cerr << "No cues extracted.\n";
        return 5;
    }

    // Sort cues by start time
    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) {
        if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
        //if (a.end_ms != b.end_ms) 
            return a.end_ms < b.end_ms;
        //return a.text < b.text;
        });

    vector<Cue>& merged = cues;

    // Merge overlapping/adjacent cues if texts are same or if they overlap
    /*
    vector<Cue> merged;
    for (auto& c : cues) {
        if (merged.empty()) { merged.push_back(c); continue; }
        Cue& last = merged.back();
        if (c.start_ms <= last.end_ms + 250) {
            // overlapping or very close (250ms tolerance) -> merge
            if (c.text == last.text) {
                last.end_ms = (std::max)(last.end_ms, c.end_ms);
            }
            else {
                // join texts with newline but extend end time
                last.text = last.text + "\n" + c.text;
                last.end_ms = (std::max)(last.end_ms, c.end_ms);
            }
        }
        else {
            merged.push_back(c);
        }
    }
    */

    // Collapse duplicate consecutive lines inside each cue and trim
    for (auto& m : merged) {
        // collapse consecutive identical lines
        std::istringstream iss(m.text);
        string out, line, prev;
        bool first = true;
        while (std::getline(iss, line)) {
            if (line == prev) continue;
            if (!first) out += "\n";
            out += line;
            prev = line;
            first = false;
        }
        m.text = trim(out);
    }

    // Build output filename: same base name with .srt
    size_t dot = infile.find_last_of('.');
    string outfile;
    if (dot == string::npos) outfile = infile + ".srt";
    else outfile = infile.substr(0, dot) + ".srt";

    std::ofstream ofs(outfile, std::ios::out | std::ios::trunc);
    if (!ofs) {
        std::cerr << "Cannot write output file: " << outfile << "\n";
        return 6;
    }

    // Write SRT
    int idx = 1;
    for (auto& m : merged) {
        ofs << idx++ << "\n";
        ofs << fmt_srt_time(m.start_ms) << " --> " << fmt_srt_time(m.end_ms) << "\n";
        // ensure CRLF not necessary; use LF
        ofs << m.text << "\n\n";
    }
    ofs.close();

    std::cout << "Extracted " << merged.size() << " cues to: " << outfile << "\n";
    return 0;
}
