/*
 * ia.cpp — inferencia do Edge Impulse, isolada do main.c
 */
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

extern "C" {

/* Recebe um buffer de 166*3 floats, roda a IA, devolve o indice do gesto.
   0 = idle, 1 = updown, 2 = wave, -1 = erro */
int ia_classificar(float *features, int n, float *confianca_out, const char **label_out) {
    signal_t signal;
    numpy::signal_from_buffer(features, n, &signal);

    ei_impulse_result_t result;
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) {
        return -1;
    }

    int best = 0;
    float best_v = 0.0f;
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (result.classification[i].value > best_v) {
            best_v = result.classification[i].value;
            best = i;
        }
    }

    *confianca_out = best_v;
    *label_out = result.classification[best].label;
    return best;
}

int ia_window_size(void) {
    return EI_CLASSIFIER_RAW_SAMPLE_COUNT;  /* 166 */
}

}