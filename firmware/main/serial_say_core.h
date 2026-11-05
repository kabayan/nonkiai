#pragma once
// USB シリアルの `say` の行の解釈と、発話ごとの 1 行ログの書式（DLS-159）。
// ESP-IDF にも FreeRTOS にも依存しない（ファームとホストの試験 tools/test_serial_say.cpp で同じ実装を使う）。
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace nk {

enum SayCmdKind { kSayEmpty, kSaySay, kSayHelp, kSayUnknown, kSayTooLong, kSayMode, kSayStatus, kSayInvalidMode };
struct SayCmd {
    SayCmdKind kind = kSayEmpty;
    int mode = -1;
    bool verbose = false;       // say -v <文>: ids の記号列と分かち書きも出す
    const char *text = nullptr; // line の中を指す（NUL 終端）
};

constexpr size_t kSayLineMax = 192;   // 受け付ける 1 行の長さ（改行を除く）。超えた行は捨てる
constexpr size_t kSayTextMax = 127;   // 文の UTF-8 の長さの上限（voice_player のメッセージ 128 B に NUL を含めて収める）

inline bool say_space(char c) { return c == ' ' || c == '\t'; }

// 行を解釈する。line は書き換える（末尾の CR / LF / 空白を落とす）。
//   say <文>        文を喋らせる
//   say -v <文>     喋らせ、ids の記号列と分かち書きも出す
//   help / ?        使い方
inline SayCmd parse_say_line(char *line) {
    SayCmd cmd;
    size_t n = std::strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || say_space(line[n - 1]))) line[--n] = '\0';
    const char *p = line;
    while (say_space(*p)) ++p;
    if (*p == '\0') { cmd.kind = kSayEmpty; return cmd; }
    if (std::strcmp(p, "help") == 0 || std::strcmp(p, "?") == 0) { cmd.kind = kSayHelp; return cmd; }
    if (std::strcmp(p, "status") == 0 || std::strcmp(p, "mode") == 0) { cmd.kind = kSayStatus; return cmd; }
    if (std::strncmp(p, "mode", 4) == 0 && say_space(p[4])) {
        p += 4; while (say_space(*p)) ++p;
        if (std::strcmp(p, "toy") == 0 || std::strcmp(p, "1") == 0) cmd.mode = 1;
        else if (std::strcmp(p, "plush") == 0 || std::strcmp(p, "0") == 0) cmd.mode = 0;
        cmd.kind = cmd.mode < 0 ? kSayInvalidMode : kSayMode; return cmd;
    }
    if (std::strncmp(p, "say", 3) != 0 || !(p[3] == '\0' || say_space(p[3]))) { cmd.kind = kSayUnknown; return cmd; }
    p += 3;
    while (say_space(*p)) ++p;
    if (std::strncmp(p, "-v", 2) == 0 && (p[2] == '\0' || say_space(p[2]))) {
        cmd.verbose = true;
        p += 2;
        while (say_space(*p)) ++p;
    }
    if (*p == '\0') { cmd.kind = kSayHelp; return cmd; }
    if (std::strlen(p) > kSayTextMax) { cmd.kind = kSayTooLong; return cmd; }
    cmd.kind = kSaySay;
    cmd.text = p;
    return cmd;
}

// 1 発話ぶんの数字（sano_utt_stats_t と voice_player の計測から埋める）。時間は ms、音声長は s。
struct SayReport {
    const char *text = "";
    const char *route = "";          // かな / 辞書 / 拒否（saan_g2p_classify の分類）
    const char *g2p_status = "OK";   // G2P の結果（OK 以外は理由）
    int ids = 0, segments = 0;
    double g2p_ms = 0, init_ms = 0, pull_ms = 0, audio_s = 0;
    int pulls = 0, full_pulls = 0, over_budget = 0;
    double full_med_ms = 0, full_mean_ms = 0, chunk_ms = 0;
    int underruns = 0;               // 再生が合成に追いついて、スピーカーのキューが空になった回数
    double start_ms = -1;            // 要求から最初の PCM をスピーカーへ渡すまで。渡していなければ負
    bool completed = false;          // 最後まで合成して再生キューに積めた
};

// 1 行のログ。定常 xRT は満チャンク pull が 1 回も無い短い発話では n/a（0.000 と出すと速く見える）。
// 出力先が小さければ切り詰める（snprintf の仕様どおり NUL 終端）。切り詰めは意図なのでその警告は外す。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
inline int format_say_log(char *out, size_t cap, const SayReport &r) {
    char steady[192], start[24], whole[16];
    if (r.full_pulls > 0 && r.chunk_ms > 0)
        std::snprintf(steady, sizeof steady, "%.3f（満チャンク %d 回の中央値 %.2f ms / %.2f ms。平均なら %.3f）",
                      r.full_med_ms / r.chunk_ms, r.full_pulls, r.full_med_ms, r.chunk_ms, r.full_mean_ms / r.chunk_ms);
    else
        std::snprintf(steady, sizeof steady, "n/a（満チャンク pull が 0 回）");
    if (r.start_ms >= 0) std::snprintf(start, sizeof start, "%.0f ms", r.start_ms);
    else std::snprintf(start, sizeof start, "n/a");
    if (r.audio_s > 0) std::snprintf(whole, sizeof whole, "%.3f", (r.init_ms + r.pull_ms) / 1000.0 / r.audio_s);
    else std::snprintf(whole, sizeof whole, "n/a");
    return std::snprintf(out, cap,
        "[SAY] 「%s」 分類=%s G2P=%s ids=%d 区間=%d | G2P %.1f ms / 合成 %.1f ms（init %.1f + pull %.1f）/ 音声 %.3f s"
        " | 発話全体 xRT=%s（(init+pull)/音声） 定常 xRT=%s | アンダーラン=%d 予算超過 pull=%d/%d | 鳴らし始めまで=%s | %s",
        r.text, r.route, r.g2p_status, r.ids, r.segments, r.g2p_ms, r.init_ms + r.pull_ms, r.init_ms, r.pull_ms, r.audio_s,
        whole, steady, r.underruns, r.over_budget, r.pulls, start, r.completed ? "完了" : "中断または失敗");
}
#pragma GCC diagnostic pop

} // namespace nk
