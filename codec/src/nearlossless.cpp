#include "nearlossless.h"

namespace rcv {

void init_near_tables(int n, NearTables* t) {
    t->step = 2 * n + 1;
    for (int e = -255; e <= 255; ++e) {
        t->q[e + 255] = int8_t(near_quantise(e, n));
        t->qstep[e + 255] = int16_t(t->q[e + 255] * t->step);
    }
    for (int s = 0; s < 256; ++s) t->dq[s] = int16_t(int8_t(uint8_t(s)) * t->step);
}

}  // namespace rcv
