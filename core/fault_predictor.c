#include "fault_predictor.h"

#include <math.h>
#include <stddef.h>

#if FAULT_MODEL_LOADED
/* Safe if the header was already force-included (laptop/tools/evaluate.py
 * passes -include <weights header>): it has include guards. */
#include "fault_predictor_weights.h"

/* Loud compile-time guard: the weights were trained on exactly the
 * feature vector core/feature_spec.h describes. If someone regenerates
 * one without the other, refuse to build instead of reading past arrays. */
_Static_assert(FEATURE_SPEC_NUM_INPUTS == (int)(sizeof(scaler_mean) / sizeof(scaler_mean[0])),
               "fault_predictor_weights.h input_dim != FEATURE_SPEC_NUM_INPUTS");
_Static_assert(FAULT_PREDICTOR_OUTPUT_DIM == NUM_OPERATIONAL_FAULT_CLASSES,
               "weights output dim != NUM_OPERATIONAL_FAULT_CLASSES");
#endif

FaultPredictorScores_t fault_predictor_run(const float evidence[FEATURE_SPEC_NUM_INPUTS])
{
    FaultPredictorScores_t scores = {0};

    if(evidence == NULL)
        return scores;

#if FAULT_MODEL_LOADED
    /* evidence is ALREADY scaled by feature_pack.c using feature_spec.h's
     * scaler. Do NOT apply scaler_mean/scaler_std again here.
     * BatchNorm is folded into W1/b1 and W2/b2 by export.py. */
    float h1[FAULT_PREDICTOR_HIDDEN1_DIM];
    float h2[FAULT_PREDICTOR_HIDDEN2_DIM];

    for(int j = 0; j < FAULT_PREDICTOR_HIDDEN1_DIM; j++)
    {
        float acc = b1[j];
        for(int i = 0; i < FEATURE_SPEC_NUM_INPUTS; i++)
            acc += W1[j][i] * evidence[i];
        h1[j] = acc > 0.0f ? acc : 0.0f;
    }

    for(int j = 0; j < FAULT_PREDICTOR_HIDDEN2_DIM; j++)
    {
        float acc = b2[j];
        for(int i = 0; i < FAULT_PREDICTOR_HIDDEN1_DIM; i++)
            acc += W2[j][i] * h1[i];
        h2[j] = acc > 0.0f ? acc : 0.0f;
    }

    for(int k = 0; k < FAULT_PREDICTOR_OUTPUT_DIM; k++)
    {
        float acc = b3[k];
        for(int i = 0; i < FAULT_PREDICTOR_HIDDEN2_DIM; i++)
            acc += W3[k][i] * h2[i];
        scores.score[k] = 1.0f / (1.0f + expf(-acc));   /* independent sigmoid per class */
    }
#else
    (void)evidence;
#endif

    return scores;
}
