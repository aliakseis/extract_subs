// extract_subs.cpp
// Modernized version: uses avcodec_send_packet / avcodec_receive_subtitle,
// RAII wrappers, and improved subtitle sanitizing (no std::regex).
//
// Compile:
// g++ -std=c++17 -O2 -o extract_subs extract_subs.cpp `pkg-config --cflags --libs libavformat libavcodec libavutil`

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

// ASS plaintext extraction (keeps the basic semantics of removing override tags etc.)
// We reuse ass_to_plaintext idea but simplified: we'll reuse the provided implementation idea.
std::string ass_to_plaintext(const char* in) {
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
                in_drawing = false;
                while (in[0] >= '0' && in[0] <= '9') {
                    if (in[0] != '0') in_drawing = true;
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
    if (in_tag) {
        result += open_tag_pos;
    }
    return result;
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
        std::cerr << "Usage: extract_subs [ -o outfile.srt ] [ -sid stream_index ] [ -lang xx ] <video-file>\n";
        return 1;
    }

    string infile;
    string forced_out;
    int forced_sid = -1;
    string forced_lang;

    // Simple arg parsing
    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a == "-o" && i + 1 < argc) { forced_out = argv[++i]; continue; }
        if (a == "-sid" && i + 1 < argc) { forced_sid = std::atoi(argv[++i]); continue; }
        if (a == "-lang" && i + 1 < argc) { forced_lang = argv[++i]; continue; }
        if (a == "--") { if (i + 1 < argc) infile = argv[++i]; break; }
        if (a.size() && a[0] == '-') {
            std::cerr << "Unknown option: " << a << "\n";
            return 1;
        }
        infile = a;
    }
    if (infile.empty()) { std::cerr << "No input file specified.\n"; return 1; }

    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();

    AVFormatContext* fmt = nullptr;
    int err = avformat_open_input(&fmt, infile.c_str(), nullptr, nullptr);
    if (err < 0) {
        std::cerr << "Error: cannot open file: " << infile << " -> " << av_err_to_string(err) << "\n";
        return 2;
    }
    // Ensure cleanup on exit
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> fmt_guard(fmt, [](AVFormatContext* p) { avformat_close_input(&p); });

    if ((err = avformat_find_stream_info(fmt, nullptr)) < 0) {
        std::cerr << "Error: cannot find stream info -> " << av_err_to_string(err) << "\n";
        return 3;
    }

    string syslang = forced_lang.empty() ? get_system_language() : forced_lang;
    std::transform(syslang.begin(), syslang.end(), syslang.begin(), ::tolower);
    if (syslang.empty()) syslang = "en";

    // Collect subtitle streams
    vector<int> sub_streams;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            sub_streams.push_back((int)i);
    }

    if (sub_streams.empty()) {
        std::cerr << "No subtitle streams in file.\n";
        return 4;
    }

    vector<int> candidate_streams;
    if (forced_sid >= 0) {
        // validate
        if (forced_sid < 0 || (size_t)forced_sid >= fmt->nb_streams) {
            std::cerr << "Invalid -sid " << forced_sid << "\n";
            return 1;
        }
        if (fmt->streams[forced_sid]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            std::cerr << "-sid refers to non-subtitle stream\n";
            return 1;
        }
        candidate_streams.push_back(forced_sid);
    }
    else {
        // find streams matching language
        for (int si : sub_streams) {
            AVDictionaryEntry* tag = av_dict_get(fmt->streams[si]->metadata, "language", nullptr, 0);
            string lang = tag ? string(tag->value) : "";
            std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);
            if (!lang.empty()) {
                if (lang == syslang || lang.find(syslang) == 0) {
                    candidate_streams.push_back(si);
                    continue;
                }
                if (lang.size() >= 2 && syslang.size() >= 2 && lang.substr(0, 2) == syslang.substr(0, 2)) {
                    candidate_streams.push_back(si);
                    continue;
                }
            }
            else {
                // keep no-language as fallback
            }
        }
        // If none matched explicitly, pick all subtitle streams as fallback
        if (candidate_streams.empty()) {
            for (int si : sub_streams) candidate_streams.push_back(si);
        }
    }

    if (candidate_streams.empty()) {
        std::cerr << "No candidate subtitle streams found.\n";
        return 5;
    }

    vector<Cue> cues;

    // Prepare packet and loop reading entire file once, but decode selected streams.
    // We'll read frames and dispatch packets by stream index.
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) { std::cerr << "av_packet_alloc failed\n"; return 1; }
    std::unique_ptr<AVPacket, void(*)(AVPacket*)> pkt_guard(pkt, [](AVPacket* p) { av_packet_free(&p); });

    // For each candidate stream prepare decoder contexts and track them
    struct StreamDecoder {
        int stream_index;
        AVCodecContext* cctx;
    };
    vector<StreamDecoder> decs;
    for (int si : candidate_streams) {
        AVStream* st = fmt->streams[si];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            std::cerr << "Warning: no decoder for stream " << si << "\n";
            continue;
        }
        AVCodecContext* cctx = avcodec_alloc_context3(dec);
        if (!cctx) { std::cerr << "Warning: cannot alloc codec context for stream " << si << "\n"; continue; }
        if ((err = avcodec_parameters_to_context(cctx, st->codecpar)) < 0) {
            std::cerr << "Warning: cannot copy codec params for stream " << si << " -> " << av_err_to_string(err) << "\n";
            avcodec_free_context(&cctx);
            continue;
        }
        if ((err = avcodec_open2(cctx, dec, nullptr)) < 0) {
            std::cerr << "Warning: cannot open decoder for stream " << si << " -> " << av_err_to_string(err) << "\n";
            avcodec_free_context(&cctx);
            continue;
        }
        decs.push_back({ si, cctx });
    }

    if (decs.empty()) {
        std::cerr << "No usable subtitle decoders available.\n";
        return 6;
    }

    // We'll read whole file and feed packets to decoders for only those streams.
    // Note: av_read_frame reads sequentially; we feed packets to appropriate decoder(s).
    while ((err = av_read_frame(fmt, pkt)) >= 0) {
        for (auto& sd : decs) {
            if (pkt->stream_index != sd.stream_index) continue;
            // send packet
            int s = avcodec_send_packet(sd.cctx, pkt);
            if (s < 0) {
                // In subtitle decoding, some packets may not be decodeable; report but continue
                // do not treat as fatal
                //std::cerr << "Warning: avcodec_send_packet failed for stream " << sd.stream_index << ": " << av_err_to_string(s) << "\n";
                continue;
            }
            // receive all subtitles produced by this packet
            while (true) {
                AVSubtitle sub;
                avsubtitle_free(&sub); // ensure clean
                memset(&sub, 0, sizeof(sub));
                int recv = avcodec_receive_subtitle(sd.cctx, &sub);
                if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) {
                    break;
                }
                if (recv < 0) {
                    // decode error: skip
                    //std::cerr << "Warning: avcodec_receive_subtitle error: " << av_err_to_string(recv) << "\n";
                    break;
                }
                // Build accumulated text for this subtitle (concatenate rects)
                string accumulated;
                for (unsigned r = 0; r < sub.num_rects; ++r) {
                    AVSubtitleRect* rect = sub.rects[r];
                    if (!rect) continue;
                    string text;
                    if (rect->ass && strlen(rect->ass) > 0) {
                        text = ass_to_plaintext(rect->ass);
                    }
                    else if (rect->text && strlen(rect->text) > 0) {
                        text = rect->text;
                    }
                    else if (rect->sub_text && strlen(rect->sub_text) > 0) {
                        // older field name
                        text = rect->sub_text;
                    }
                    else {
                        continue;
                    }
                    if (!accumulated.empty()) accumulated += "\n";
                    accumulated += text;
                }

                // Convert pts/display time to ms
                int64_t start_ms = AV_NOPTS_VALUE;
                int64_t end_ms = AV_NOPTS_VALUE;
                AVStream* st = fmt->streams[sd.stream_index];
                if (sub.pts != AV_NOPTS_VALUE) {
                    AVRational tb = st->time_base;
                    if (tb.num && tb.den) start_ms = av_rescale_q(sub.pts, tb, AVRational{ 1,1000 });
                    else start_ms = av_rescale_q(sub.pts, AVRational{ 1,AV_TIME_BASE }, AVRational{ 1,1000 });
                }
                else if (pkt->pts != AV_NOPTS_VALUE) {
                    AVRational tb = st->time_base;
                    start_ms = (tb.num && tb.den) ? av_rescale_q(pkt->pts, tb, AVRational{ 1,1000 }) : 0;
                }
                else {
                    start_ms = 0;
                }
                if (sub.end_display_time > 0) {
                    if (start_ms != AV_NOPTS_VALUE) end_ms = start_ms + sub.end_display_time;
                    else end_ms = sub.end_display_time;
                }
                else {
                    // try to use pkt duration
                    if (pkt->duration > 0 && pkt->pts != AV_NOPTS_VALUE) {
                        AVRational tb = st->time_base;
                        int64_t pkt_ms = (tb.num && tb.den) ? av_rescale_q(pkt->pts, tb, AVRational{ 1,1000 }) : 0;
                        int64_t dur_ms = (tb.num && tb.den) ? av_rescale_q(pkt->duration, tb, AVRational{ 1,1000 }) : 2000;
                        start_ms = pkt_ms;
                        end_ms = pkt_ms + dur_ms;
                    }
                    else {
                        if (start_ms == AV_NOPTS_VALUE) start_ms = 0;
                        end_ms = start_ms + 2000;
                    }
                }
                if (start_ms == AV_NOPTS_VALUE) start_ms = 0;
                if (end_ms == AV_NOPTS_VALUE || end_ms < start_ms) end_ms = start_ms + 2000;

                string clean = sanitize_sub_text_fast(accumulated);
                if (!clean.empty()) {
                    Cue c; c.start_ms = start_ms; c.end_ms = end_ms; c.text = clean;
                    cues.push_back(std::move(c));
                }

                avsubtitle_free(&sub);
            } // receive loop
        } // for decs
        av_packet_unref(pkt);
    }

    // flush decoders (send null packet)
    for (auto& sd : decs) {
        avcodec_send_packet(sd.cctx, nullptr);
        while (true) {
            AVSubtitle sub;
            memset(&sub, 0, sizeof(sub));
            int recv = avcodec_receive_subtitle(sd.cctx, &sub);
            if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) break;
            if (recv < 0) break;
            string accumulated;
            for (unsigned r = 0; r < sub.num_rects; ++r) {
                AVSubtitleRect* rect = sub.rects[r];
                if (!rect) continue;
                string text;
                if (rect->ass && strlen(rect->ass) > 0) {
                    text = ass_to_plaintext(rect->ass);
                }
                else if (rect->text && strlen(rect->text) > 0) {
                    text = rect->text;
                }
                else if (rect->sub_text && strlen(rect->sub_text) > 0) {
                    text = rect->sub_text;
                }
                else {
                    continue;
                }
                if (!accumulated.empty()) accumulated += "\n";
                accumulated += text;
            }
            int64_t start_ms = AV_NOPTS_VALUE, end_ms = AV_NOPTS_VALUE;
            if (sub.pts != AV_NOPTS_VALUE) {
                AVRational tb = fmt->streams[sd.stream_index]->time_base;
                start_ms = (tb.num && tb.den) ? av_rescale_q(sub.pts, tb, AVRational{ 1,1000 }) : 0;
            }
            else start_ms = 0;
            if (sub.end_display_time > 0) {
                end_ms = start_ms + sub.end_display_time;
            }
            else end_ms = start_ms + 2000;
            if (start_ms == AV_NOPTS_VALUE) start_ms = 0;
            if (end_ms == AV_NOPTS_VALUE || end_ms < start_ms) end_ms = start_ms + 2000;
            string clean = sanitize_sub_text_fast(accumulated);
            if (!clean.empty()) cues.push_back({ start_ms, end_ms, clean });
            avsubtitle_free(&sub);
        }
    }

    // free codec contexts
    for (auto& sd : decs) {
        avcodec_free_context(&sd.cctx);
    }

    if (cues.empty()) {
        std::cerr << "No cues extracted.\n";
        return 7;
    }

    // Sort cues by start time
    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) {
        if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
        if (a.end_ms != b.end_ms) return a.end_ms < b.end_ms;
        return a.text < b.text;
        });

    // Collapse duplicate consecutive lines inside each cue and trim
    for (auto& m : cues) {
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

    // Merge very-close/overlapping cues (simple merge: join text if overlapping or within 250ms)
    vector<Cue> merged;
    for (auto& c : cues) {
        if (merged.empty()) { merged.push_back(c); continue; }
        Cue& last = merged.back();
        if (c.start_ms <= last.end_ms + 250) {
            // merge
            if (c.text == last.text) {
                last.end_ms = std::max(last.end_ms, c.end_ms);
            }
            else {
                last.text = last.text + "\n" + c.text;
                last.end_ms = std::max(last.end_ms, c.end_ms);
            }
        }
        else {
            merged.push_back(c);
        }
    }

    // Build output filename if not forced
    string outfile;
    if (!forced_out.empty()) outfile = forced_out;
    else {
        size_t dot = infile.find_last_of('.');
        if (dot == string::npos) outfile = infile + ".srt";
        else outfile = infile.substr(0, dot) + ".srt";
    }

    std::ofstream ofs(outfile, std::ios::out | std::ios::trunc);
    if (!ofs) {
        std::cerr << "Cannot write output file: " << outfile << "\n";
        return 8;
    }

    // Write SRT
    int idx = 1;
    for (auto& m : merged) {
        ofs << idx++ << "\n";
        ofs << fmt_srt_time(m.start_ms) << " --> " << fmt_srt_time(m.end_ms) << "\n";
        ofs << m.text << "\n\n";
    }
    ofs.close();

    std::cout << "Extracted " << merged.size() << " cues to: " << outfile << "\n";
    return 0;
}
