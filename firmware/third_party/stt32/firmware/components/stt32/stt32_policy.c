#include "stt32_policy.h"

#include <float.h>

#include "stt32_intents.h"

bool stt32_policy_stops(stt32_listen_mode_t mode, stt32_decision_t decision) {
    switch (mode) {
    case STT32_LISTEN_ONE_UTTERANCE:
        return true;
    case STT32_LISTEN_ONE_COMMAND:
        return decision != STT32_REJECTED;
    case STT32_LISTEN_CONTINUOUS:
    default:
        return false;
    }
}

void stt32_policy_reject(stt32_reject_reason_t reason, stt32_result_t *result) {
    result->event = STT32_EVENT_UTTERANCE;
    result->decision = STT32_REJECTED;
    result->reject_reason = reason;
    result->semantic_id = STT32_SEMANTIC_ID_INVALID;
    result->expression_id = STT32_EXPRESSION_ID_INVALID;
    result->semantic_score = 0.0f;
    result->semantic_margin = 0.0f;
    result->expression_score = 0.0f;
    result->expression_margin = 0.0f;
}

void stt32_policy_decide(const float *scores, size_t count, const stt32_semantic_id_t *semantic_ids,
                         stt32_thresholds_t thresholds, stt32_result_t *result) {
    stt32_policy_reject(STT32_REJECT_INFERENCE_ERROR, result);
    if (scores == NULL || semantic_ids == NULL || count < 2) {
        return;
    }
    /* First-max tie-break (numpy argmax), as the diagnostic top-3. */
    size_t best = 0;
    for (size_t i = 1; i < count; ++i) {
        if (scores[i] > scores[best]) {
            best = i;
        }
    }
    float competing = -FLT_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (i != best && scores[i] > competing) {
            competing = scores[i];
        }
    }
    result->semantic_score = scores[best];
    result->semantic_margin = scores[best] - competing;

    if (result->semantic_score < thresholds.min_score) {
        result->reject_reason = STT32_REJECT_LOW_SCORE;
        return;
    }
    if (result->semantic_margin < thresholds.min_margin) {
        result->reject_reason = STT32_REJECT_LOW_MARGIN;
        return;
    }
    /* No expression model is adopted yet (DLS-179): semantic acceptance is the
     * complete success result. */
    result->decision = STT32_SEMANTIC_ONLY;
    result->reject_reason = STT32_REJECT_NONE;
    result->semantic_id = semantic_ids[best];
}

static bool in_mask(uint32_t mask, stt32_semantic_id_t id) {
    return mask == 0 || (id < 32u && ((mask >> id) & 1u) != 0u);
}

void stt32_policy_decide_set(const float *scores, size_t count, const stt32_semantic_id_t *semantic_ids,
                             stt32_thresholds_t thresholds, const stt32_listen_options_t *set,
                             stt32_result_t *result) {
    if (set == NULL || set->accept_rule == STT32_ACCEPT_THRESHOLDS) {
        stt32_policy_decide(scores, count, semantic_ids, thresholds, result);
        return;
    }
    stt32_policy_reject(STT32_REJECT_INFERENCE_ERROR, result);
    if (scores == NULL || semantic_ids == NULL || count < 2 ||
        (set->accept_rule != STT32_ACCEPT_TOP1_IN_SET && set->accept_rule != STT32_ACCEPT_SET_MAX_SCORE)) {
        return;
    }
    const uint32_t mask = set->intent_mask;
    /* Overall top-1 and in-set top-1 / top-2 (first-max tie-break, as stt32_policy_decide). */
    size_t best = 0;
    for (size_t i = 1; i < count; ++i) {
        if (scores[i] > scores[best]) {
            best = i;
        }
    }
    bool have = false;
    size_t sbest = 0;
    for (size_t i = 0; i < count; ++i) {
        if (in_mask(mask, semantic_ids[i]) && (!have || scores[i] > scores[sbest])) {
            sbest = i;
            have = true;
        }
    }
    float scompeting = 0.0f; /* a one-member set: the second is 0 */
    bool have2 = false;
    for (size_t i = 0; i < count; ++i) {
        if (have && i != sbest && in_mask(mask, semantic_ids[i]) && (!have2 || scores[i] > scompeting)) {
            scompeting = scores[i];
            have2 = true;
        }
    }

    if (set->accept_rule == STT32_ACCEPT_TOP1_IN_SET) {
        float competing = -FLT_MAX;
        for (size_t i = 0; i < count; ++i) {
            if (i != best && scores[i] > competing) {
                competing = scores[i];
            }
        }
        if (!in_mask(mask, semantic_ids[best])) {
            result->semantic_score = scores[best];
            result->semantic_margin = scores[best] - competing;
            result->reject_reason = STT32_REJECT_OUT_OF_SET;
            return;
        }
        /* best is in the set, so it is the in-set maximum too (same first-max order). */
    } else if (!have || !(scores[sbest] >= set->set_min_score)) {
        if (have) {
            result->semantic_score = scores[sbest];
            result->semantic_margin = scores[sbest] - scompeting;
        }
        result->reject_reason = STT32_REJECT_LOW_SCORE;
        return;
    }
    result->semantic_score = scores[sbest];
    result->semantic_margin = scores[sbest] - scompeting;
    result->decision = STT32_SEMANTIC_ONLY;
    result->reject_reason = STT32_REJECT_NONE;
    result->semantic_id = semantic_ids[sbest];
}

bool stt32_policy_has_wake_class(const stt32_semantic_id_t *semantic_ids, size_t count) {
    if (semantic_ids == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (semantic_ids[i] == (stt32_semantic_id_t)STT32_INTENT_NONKICHAN) {
            return true;
        }
    }
    return false;
}

void stt32_policy_decide_wake(const float *scores, size_t count, const stt32_semantic_id_t *semantic_ids,
                              stt32_thresholds_t wake, uint32_t max_rank, stt32_result_t *result) {
    stt32_policy_reject(STT32_REJECT_INFERENCE_ERROR, result);
    if (scores == NULL || semantic_ids == NULL || count < 2) {
        return;
    }
    size_t w = count;
    for (size_t i = 0; i < count; ++i) {
        if (semantic_ids[i] == (stt32_semantic_id_t)STT32_INTENT_NONKICHAN) {
            w = i;
            break;
        }
    }
    if (w == count) {
        return;
    }
    float other = -FLT_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (i != w && scores[i] > other) {
            other = scores[i];
        }
    }
    result->semantic_score = scores[w];
    result->semantic_margin = scores[w] - other;
    if (!(scores[w] >= wake.min_score)) {
        result->reject_reason = STT32_REJECT_LOW_SCORE;
        return;
    }
    if (max_rank > 0) {
        /* rank = 1 + the number of classes strictly above the wake word (a tie favours the wake word) */
        size_t rank = 1;
        for (size_t i = 0; i < count; ++i) {
            if (i != w && scores[i] > scores[w]) {
                ++rank;
            }
        }
        if (rank > max_rank) {
            result->reject_reason = STT32_REJECT_LOW_RANK;
            return;
        }
    }
    result->event = STT32_EVENT_WAKE;
    result->decision = STT32_SEMANTIC_ONLY;
    result->reject_reason = STT32_REJECT_NONE;
    /* semantic_id stays INVALID (stt32_policy_reject): a wake word is not a command. */
}
