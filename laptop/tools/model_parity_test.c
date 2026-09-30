/*
 * Model parity test: PyTorch vs the C forward pass in core/fault_predictor.c.
 *
 * Reads laptop/tools/model_parity_fixture.csv (written by
 * ml/export_parity_fixture.py: name, 75 already-scaled inputs, 5 PyTorch
 * sigmoid outputs per row), feeds every input through fault_predictor_run()
 * and requires every output within 1e-5 of PyTorch's. Prints the worst
 * absolute difference and where it happened.
 *
 * Built twice by `make check-model-parity`:
 *   -DFAULT_MODEL_LOADED=1 : the parity comparison above.
 *   default (0)            : asserts fault_predictor_run() returns exactly
 *                            0.0f for every class on every fixture row -
 *                            the "AI is off unless asked for" guarantee.
 *
 * usage: model_parity_test <fixture.csv>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fault_predictor.h"

#define TOLERANCE 1e-5f
#define LINE_MAX_LEN 4096

int main(int argc, char **argv)
{
    FILE *f;
    char line[LINE_MAX_LEN];
    int row = 0, failures = 0;
    float worst = 0.0f;
    int worst_row = -1, worst_class = -1;
    char worst_name[64] = "";

    if(argc != 2)
    {
        fprintf(stderr, "usage: %s <fixture.csv>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "r");
    if(f == NULL)
    {
        fprintf(stderr, "FAIL: cannot open %s - generate it with `python ml/export_parity_fixture.py`\n", argv[1]);
        return 1;
    }
    if(fgets(line, sizeof(line), f) == NULL) /* header */
    {
        fprintf(stderr, "FAIL: %s is empty\n", argv[1]);
        return 1;
    }

    while(fgets(line, sizeof(line), f) != NULL)
    {
        float in[FEATURE_SPEC_NUM_INPUTS];
        float expected[NUM_OPERATIONAL_FAULT_CLASSES];
        char name[64];
        char *tok, *save = NULL;
        int n = 0;
        FaultPredictorScores_t got;

        line[strcspn(line, "\r\n")] = '\0';
        if(line[0] == '\0')
            continue;

        tok = strtok_r(line, ",", &save);
        if(tok == NULL)
            continue;
        snprintf(name, sizeof(name), "%s", tok);

        while((tok = strtok_r(NULL, ",", &save)) != NULL)
        {
            if(n < FEATURE_SPEC_NUM_INPUTS)
                in[n] = strtof(tok, NULL);
            else if(n < FEATURE_SPEC_NUM_INPUTS + NUM_OPERATIONAL_FAULT_CLASSES)
                expected[n - FEATURE_SPEC_NUM_INPUTS] = strtof(tok, NULL);
            n++;
        }
        if(n != FEATURE_SPEC_NUM_INPUTS + NUM_OPERATIONAL_FAULT_CLASSES)
        {
            fprintf(stderr, "FAIL: row %d (%s) has %d numbers, expected %d - fixture is stale for this feature contract\n",
                    row, name, n, FEATURE_SPEC_NUM_INPUTS + NUM_OPERATIONAL_FAULT_CLASSES);
            return 1;
        }

        got = fault_predictor_run(in);
        for(int k = 0; k < NUM_OPERATIONAL_FAULT_CLASSES; k++)
        {
#if FAULT_MODEL_LOADED
            float d = fabsf(got.score[k] - expected[k]);
            if(!(d <= TOLERANCE)) /* also catches NaN */
            {
                printf("FAIL: %s class %d: C=%.9g pytorch=%.9g diff=%.3g\n", name, k, got.score[k], expected[k], d);
                failures++;
            }
            if(!(d <= worst))
            {
                worst = d;
                worst_row = row;
                worst_class = k;
                snprintf(worst_name, sizeof(worst_name), "%s", name);
            }
#else
            (void)expected;
            if(got.score[k] != 0.0f)
            {
                printf("FAIL: FAULT_MODEL_LOADED=0 but %s class %d scored %.9g (must be exactly 0)\n", name, k, got.score[k]);
                failures++;
            }
#endif
        }
        row++;
    }
    fclose(f);

    if(row == 0)
    {
        fprintf(stderr, "FAIL: no vectors in %s\n", argv[1]);
        return 1;
    }
#if FAULT_MODEL_LOADED
    printf("model parity: %d vectors, worst |C - PyTorch| = %.3g (vector %d '%s', class %d), tolerance %.0e\n",
           row, worst, worst_row, worst_name, worst_class, (double)TOLERANCE);
#else
    (void)worst; (void)worst_row; (void)worst_class; (void)worst_name;
    printf("model off-by-default: %d vectors, all scores exactly 0 with FAULT_MODEL_LOADED=0\n", row);
#endif
    if(failures)
    {
        printf("FAIL: %d mismatch(es)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
