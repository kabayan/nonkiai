#pragma once

/* Pure decision rules of the stt32 service (no RTOS, host-testable). */

#include "stt32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Contract §5: does a result with this decision end the listening session? */
bool stt32_policy_stops(stt32_listen_mode_t mode, stt32_decision_t decision);

/*
 * Semantic decision from a softmax vector. semantic_ids maps each model index
 * to its stable id. Fills decision, reject_reason, ids, scores and margins of
 * result; leaves session/utterance/timing fields untouched.
 */
void stt32_policy_decide(const float *scores, size_t count, const stt32_semantic_id_t *semantic_ids,
                         stt32_thresholds_t thresholds, stt32_result_t *result);

/*
 * Decision against a candidate set (DLS-1082). set->accept_rule is STT32_ACCEPT_TOP1_IN_SET or
 * STT32_ACCEPT_SET_MAX_SCORE; STT32_ACCEPT_THRESHOLDS (0) falls back to stt32_policy_decide with
 * `thresholds` (intent_mask ignored). intent_mask bit i = stt32_intent_t i, 0 = all classes; ids >= 32
 * are never in a non-zero mask. Probabilities are never renormalised over the set (DLS-003).
 * Only `intent_mask`, `accept_rule` and `set_min_score` of `set` are read.
 */
void stt32_policy_decide_set(const float *scores, size_t count, const stt32_semantic_id_t *semantic_ids,
                             stt32_thresholds_t thresholds, const stt32_listen_options_t *set,
                             stt32_result_t *result);

/*
 * Wake-word decision (DLS-251 / DLS-1082): accepted when the probability of STT32_INTENT_NONKICHAN is >=
 * wake.min_score. Fills event = WAKE, SEMANTIC_ONLY, semantic_id INVALID, semantic_score = that
 * probability and semantic_margin = it minus the largest other class on acceptance. A model without the
 * class gives REJECTED / INFERENCE_ERROR. Below the threshold: REJECTED / LOW_SCORE (score and margin kept).
 * max_rank > 0 also needs rank <= max_rank, rank = 1 + the number of classes whose probability is strictly
 * greater than the wake word's (a tie favours the wake word); otherwise REJECTED / LOW_RANK (score and
 * margin kept). max_rank == 0: the rank is not looked at.
 */
void stt32_policy_decide_wake(const float *scores, size_t count, const stt32_semantic_id_t *semantic_ids,
                              stt32_thresholds_t wake, uint32_t max_rank, stt32_result_t *result);

/* Does the model have the wake-word class (STT32_INTENT_NONKICHAN)? */
bool stt32_policy_has_wake_class(const stt32_semantic_id_t *semantic_ids, size_t count);

/* Rejection that never reached the model (too short, inference error). */
void stt32_policy_reject(stt32_reject_reason_t reason, stt32_result_t *result);

#ifdef __cplusplus
}
#endif
