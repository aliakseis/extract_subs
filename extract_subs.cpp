#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <locale>
#include <cctype>
#include <sstream>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <climits>

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
    WCHAR localeName[LOCALE_NAME_MAX_LENGTH] = { 0 };
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        char buf[LOCALE_NAME_MAX_LENGTH] = { 0 };
        int n = WideCharToMultiByte(CP_UTF8, 0, localeName, -1, buf, (int)sizeof(buf), nullptr, nullptr);
        if (n > 0) return normalize_lang(std::string(buf));
    }
    LANGID lid = GetUserDefaultLangID();
    if (lid != 0) {
        LCID lcid = MAKELCID(lid, SORT_DEFAULT);
        char buf[16] = { 0 };
        if (GetLocaleInfoA(lcid, LOCALE_SISO639LANGNAME, buf, (int)sizeof(buf)) > 0) {
            return normalize_lang(std::string(buf));
        }
    }
    std::string langEnv;
    if (const char* env = std::getenv("LANG")) langEnv = env;
    if (!langEnv.empty()) return normalize_lang(langEnv);
    try {
        std::locale loc("");
        std::string name = loc.name();
        if (!name.empty()) return normalize_lang(name);
    }
    catch (...) {}
    return "en";
}
#else
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
    std::string langEnv;
    if (const char* env = std::getenv("LANG")) langEnv = env;
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

static string html_unescape_simple(const string& s) {
    // Only common entities supported; extend if needed.
    string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 4; continue; }
            if (s.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 3; continue; }
            if (s.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 3; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 5; continue; }
            if (s.compare(i, 5, "&#39;") == 0) { out.push_back('\''); i += 4; continue; }
            // numeric entity &#nnn;
            if (i + 3 < s.size() && s[i + 1] == '#') {
                size_t k = i + 2;
                int code = 0;
                while (k < s.size() && isdigit((unsigned char)s[k])) { code = code * 10 + (s[k] - '0'); ++k; }
                if (k < s.size() && s[k] == ';') {
                    if (code > 0 && code < 0x10FFFF) {
                        // naive: only basic ASCII/Latin-1
                        if (code < 128) out.push_back((char)code);
                        // else replace with '?'
                        else out.push_back('?');
                        i = k;
                        continue;
                    }
                }
            }
            // unknown entity, copy '&' literally
            out.push_back('&');
        }
        else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Remove ASS override blocks {...}, remove <...> HTML tags, handle \N,\n,\h, collapse multiple spaces and blank lines,
// remove \pN drawing switches, keep plain text. Implemented with manual parsing for performance.
static string sanitize_sub_text_fast(const string& in) {
    string out;
    out.reserve(in.size());
    const char* p = in.c_str();
    size_t len = in.size();
    size_t i = 0;

    // First pass: remove {...} and <...> and handle backslash escapes like \N \n \h \pN
    bool in_brace = false;
    bool in_angle = false;
    bool in_drawing = false; // when \pN specifies drawing
    while (i < len) {
        char c = p[i];
        if (in_brace) {
            if (c == '}') { in_brace = false; }
            // otherwise skip
            ++i;
            continue;
        }
        if (in_angle) {
            if (c == '>') { in_angle = false; }
            ++i;
            continue;
        }
        if (c == '{') {
            in_brace = true;
            ++i;
            continue;
        }
        if (c == '<') {
            in_angle = true;
            ++i;
            continue;
        }
        if (c == '\\') {
            // handle escape sequences
            if (i + 1 < len) {
                char n = p[i + 1];
                if (n == 'N' || n == 'n') { out.push_back('\n'); i += 2; continue; }
                if (n == 'h') { out.push_back(' '); i += 2; continue; }
                if (n == 'p') {
                    // \pN where N is digits (drawing). Enter drawing until \p0 or digits follow?
                    i += 2;
                    // skip digits following \p
                    bool anyNonZero = false;
                    while (i < len && isdigit((unsigned char)p[i])) {
                        if (p[i] != '0') anyNonZero = true;
                        ++i;
                    }
                    // We treat drawing as skipped — drawing text should be skipped until braces are closed in ASS,
                    // but here we simply stop outputting characters until a non-drawing context. For simplicity
                    // we ignore a special state and continue parsing.
                    continue;
                }
                // other backslash sequences \xxx: drop the backslash, keep the next char (common)
                // e.g. \{ or \}
                out.push_back(n);
                i += 2;
                continue;
            }
            ++i;
            continue;
        }
        if (c == '\r') {
            out.push_back('\n');
            ++i;
            continue;
        }
        out.push_back(c);
        ++i;
    }

    // Second pass: unescape simple HTML entities and normalize whitespace and blank lines.
    string unescaped = html_unescape_simple(out);

    // Normalize newlines: convert CRLF/LF mixed to '\n' already done; collapse multiple spaces, trim lines.
    std::istringstream iss(unescaped);
    string line;
    string accum;
    bool firstLine = true;
    while (std::getline(iss, line)) {
        // Trim both ends
        size_t a = line.find_first_not_of(" \t");
        if (a == string::npos) {
            // blank line
            if (!firstLine && !accum.empty() && accum.back() != '\n') accum.push_back('\n');
            // ensure we only have a single blank line eventually — we'll collapse later
        }
        else {
            size_t b = line.find_last_not_of(" \t");
            string t = line.substr(a, b - a + 1);
            // Collapse multiple internal spaces/tabs to single space
            string collapsed;
            collapsed.reserve(t.size());
            bool prev_space = false;
            for (char ch : t) {
                if (ch == ' ' || ch == '\t') {
                    if (!prev_space) collapsed.push_back(' ');
                    prev_space = true;
                }
                else {
                    collapsed.push_back(ch);
                    prev_space = false;
                }
            }
            if (!firstLine) accum.push_back('\n');
            accum += collapsed;
        }
        firstLine = false;
    }
    // collapse multiple blank lines
    string final_out;
    final_out.reserve(accum.size());
    size_t pidx = 0;
    int blank_run = 0;
    while (pidx < accum.size()) {
        // detect newline sequences
        if (accum[pidx] == '\n') {
            // count newlines
            size_t q = pidx;
            while (q < accum.size() && accum[q] == '\n') ++q;
            int count = (int)(q - pidx);
            if (count >= 2) {
                // keep exactly one blank line (i.e., two newlines in sequence -> preserve as single blank line)
                final_out.append("\n\n");
            }
            else {
                final_out.push_back('\n');
            }
            pidx = q;
            continue;
        }
        else {
            final_out.push_back(accum[pidx]);
            ++pidx;
        }
    }
    // Trim outer whitespace/newlines
    return trim(final_out);
}

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

static string av_err_to_string(int errnum) {
    char buf[128] = { 0 };
    av_strerror(errnum, buf, sizeof(buf));
    return string(buf);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: extract_subs <video-file>\n";
        return 1;
    }
    string infile = argv[1];

    av_log_set_level(AV_LOG_ERROR);
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, infile.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Error opening file\n"; return 2;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::cerr << "Error: cannot find stream info\n"; avformat_close_input(&fmt); return 3;
    }

    string syslang = get_system_language();
    if (syslang.empty()) syslang = "en";

    vector<int> candidates;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        AVDictionaryEntry* tag = av_dict_get(st->metadata, "language", nullptr, 0);
        string lang = tag ? tag->value : "";
        std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);
        if (!lang.empty()) {
            if (lang == syslang || lang.find(syslang) == 0) candidates.push_back(i);
            else if (lang.size() >= 2 && syslang.size() >= 2 && lang.substr(0, 2) == syslang.substr(0, 2))
                candidates.push_back(i);
        }
        else {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) { std::cerr << "No subtitle streams found\n"; avformat_close_input(&fmt); return 4; }

    vector<Cue> cues;
    for (int si : candidates) {
        AVStream* st = fmt->streams[si];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) continue;
        AVCodecContext* cctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(cctx, st->codecpar);
        avcodec_open2(cctx, dec, nullptr);

        AVPacket pkt{}; // FIX: zero-init instead of av_init_packet
        while (av_read_frame(fmt, &pkt) >= 0) {
            if (pkt.stream_index != si) { av_packet_unref(&pkt); continue; }
            AVSubtitle sub; int got_sub = 0;
            int ret = avcodec_decode_subtitle2(cctx, &sub, &got_sub, &pkt);
            if (ret >= 0 && got_sub) {
                string text;
                for (unsigned r = 0; r < sub.num_rects; ++r) {
                    auto rect = sub.rects[r];
                    if (!rect) continue;
                    if (rect->ass && strlen(rect->ass))
                        text += fromAss(rect->ass);
                    else if (rect->text && strlen(rect->text))
                        text += rect->text;
                }
                text = sanitize_sub_text_fast(text);
                if (!text.empty()) {
                    // ---------- FIXED TIMESTAMP HANDLING ----------
                    int64_t start_ms = AV_NOPTS_VALUE;
                    int64_t end_ms = AV_NOPTS_VALUE;
                    if (sub.pts != AV_NOPTS_VALUE) {
                        AVRational tb = st->time_base;
                        if (tb.num && tb.den)
                            start_ms = av_rescale_q(sub.pts, tb, AVRational{ 1,1000 });
                        else
                            start_ms = av_rescale_q(sub.pts, AVRational{ 1,AV_TIME_BASE }, AVRational{ 1,1000 });
                    }
                    else if (pkt.pts != AV_NOPTS_VALUE) {
                        AVRational tb = st->time_base;
                        start_ms = (tb.num && tb.den) ? av_rescale_q(pkt.pts, tb, AVRational{ 1,1000 }) : av_rescale_q(pkt.pts, AVRational{ 1,AV_TIME_BASE }, AVRational{ 1,1000 });
                        if (pkt.duration > 0) {
                            int64_t dur_ms = (tb.num && tb.den) ? av_rescale_q(pkt.duration, tb, AVRational{ 1,1000 }) : 0;
                            end_ms = start_ms + dur_ms;
                        }
                        else {
                            end_ms = start_ms + 2000;
                        }
                    }
                    else {
                        start_ms = 0;
                        end_ms = start_ms + 2000;
                    }
                    if (sub.end_display_time > 0) {
                        if (start_ms != AV_NOPTS_VALUE)
                            end_ms = start_ms + sub.end_display_time;
                        else
                            end_ms = sub.end_display_time;
                    }
                    if (start_ms == AV_NOPTS_VALUE) start_ms = 0;
                    if (end_ms == AV_NOPTS_VALUE || end_ms < start_ms) end_ms = start_ms + 2000;
                    // ---------- end FIXED TIMESTAMP HANDLING ----------
                    cues.push_back({ start_ms,end_ms,text });
                }
                avsubtitle_free(&sub);
            }
            av_packet_unref(&pkt);
        }
        avcodec_free_context(&cctx);
        avformat_seek_file(fmt, -1, INT64_MIN, 0, INT64_MAX, 0);
    }
    avformat_close_input(&fmt);

    if (cues.empty()) { std::cerr << "No cues\n"; return 5; }
    std::sort(cues.begin(), cues.end(), [](auto& a, auto& b) {return a.start_ms < b.start_ms; });

    // replace extension with .srt
    string outfile = infile;
    size_t dot = outfile.find_last_of('.');
    if (dot != string::npos) outfile = outfile.substr(0, dot);
    outfile += ".srt";

    std::ofstream ofs(outfile);

    // UTF-8 BOM
    ofs.write("\xEF\xBB\xBF", 3);

    int idx = 1;
    for (auto& m : cues) {
        ofs << idx++ << "\n";
        ofs << fmt_srt_time(m.start_ms) << " --> " << fmt_srt_time(m.end_ms) << "\n";
        ofs << m.text << "\n\n";
    }
    std::cout << "Extracted " << cues.size() << " cues to " << outfile << "\n";
    return 0;
}
