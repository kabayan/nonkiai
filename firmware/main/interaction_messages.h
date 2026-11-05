#pragma once
#include <stdint.h>
#include <stddef.h>

inline constexpr const char *kNodMessages[] = {
    "ウンウン、私もそう思うよ。",
    "そうだね、そのとおりだね。",
    "ウン、すごくよくわかるよ。",
    "私もおんなじ気持ちだよ。",
    "ウンウン、それはいいね。",
    "そうそう、私もさんせいだよ。",
    "たしかに、そのとおりだね。",
    "ウン、それでいいと思うよ。",
    "そうだよね、よくわかるよ。",
    "ウンウン、私もそうするよ。",
};
inline constexpr const char *kTiltMessages[] = {
    "あれれ、なにかあったかな？",
    "んー、なにか気になるのかな？",
    "おや、ちょっと考えごとかな？",
    "なんだか、いつもと違うかな？",
    "んー、なにか迷ってるのかな？",
    "あれ、なにか見つけたのかな？",
    "もしかして、お話があるのかな？",
    "なんだろう、気になることかな？",
    "おやおや、ちょっとひと休みかな？",
    "んー、なにか言いたそうかな？",
};
inline constexpr const char *kTurnMessages[] = {
    "大変だったね。大丈夫だよ。",
    "つらかったね。無理しないでね。",
    "よくがんばったね。ひと休みしよう。",
    "そんな日もあるよ。ゆっくりいこう。",
    "あせらなくていいよ。少しずつね。",
    "今はちょっと、お休みしてもいいよ。",
    "うまくいかない日もあるよね。",
    "元気が出ないときも、あっていいよ。",
    "泣きたいときは、泣いてもいいよ。",
    "がんばりすぎなくても、大丈夫だよ。",
};
inline constexpr const char *kCompatibilityComments[8][3] = {
    {"これから仲良くなろうね。", "ちがうところも、おもしろいね。", "まだまだ、きみを知りたいな。"},
    {"仲良しへのだいいっぽだね。", "少しずつ仲良くなろうね。", "きみの好きなものも教えてね。"},
    {"ちょっぴり気が合うね。", "もっと一緒に遊びたいな。", "これからが楽しみだね。"},
    {"だんだん仲良くなれそう。", "きみのこと、気になっちゃう。", "もっと仲良くなれる予感。"},
    {"なかなかいい相性だね。", "きみといると楽しいな。", "仲良しになれそうだね。"},
    {"かなり気が合うね。", "いいお友だちになれそう。", "もっと一緒にいたいな。"},
    {"とっても仲良しだね。", "きみとなら毎日楽しそう。", "すてきなコンビになれそう。"},
    {"ぴったりの相性だね。", "最高の仲良しコンビだね。", "やったね。相性ばっちり。"},
};
inline constexpr unsigned kCompatibilityScores[] = {0, 15, 29, 43, 58, 72, 86, 100};
inline constexpr const char *kQuizQuestions[] = {
    "とんこつラーメンは好き？", "しいたけはきらい？", "のんきちゃんはかわいい？",
};
inline constexpr char kQuizIntro[] = "相性診断をするよ。はいは1回、いいえは2回タップで答えてね";
inline constexpr char kQuizRetry[] = "よくわからないからもう一回答えてね";

// Uniform rejection sampling, excluding only the previous item in this category.
inline size_t nk_choose_message(size_t count, int &previous, uint32_t (*random)()) {
    const size_t choices = count - (previous >= 0 ? 1 : 0);
    const uint32_t limit = UINT32_MAX - UINT32_MAX % choices;
    uint32_t value;
    do { value = random(); } while (value >= limit);
    size_t index = value % choices;
    if (previous >= 0 && index >= static_cast<size_t>(previous)) ++index;
    previous = static_cast<int>(index);
    return index;
}
