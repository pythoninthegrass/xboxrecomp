/* Interpolation/prediction was retired after visible replay artifacts.
 * Keep the recorder ABI for the upstream frontend, without an engine,
 * frame history, transform prediction or intermediate presentation. */
#include "hle_d3d8_record.h"
int hle_d3d8_interp_predicting;
void hle_d3d8_interp_init(void) { hle_d3d8_interp_rec = 0; }
void hle_d3d8_interp_frame_end(void) {}
void hle_d3d8_interp_finish(void) {}
void hle_d3d8_interp_report(void) {}
void hle_d3d8_interp_unsafe(void) {}
unsigned long hle_d3d8_interp_presents(void) { return 0; }
unsigned long hle_d3d8_interp_suppressed(void) { return 0; }
int hle_d3d8_interp_skip_guest(void) { return 0; }
void hle_d3d8_interp_op(void (*fn)(const void *),const void *arg,size_t size)
{ (void)fn;(void)arg;(void)size; }
void hle_d3d8_interp_draw(void (*fn)(const void *),const void *arg,size_t size,
                        const HleInterpDrawKey *key)
{ (void)fn;(void)arg;(void)size;(void)key; }
void hle_d3d8_interp_retire(void (*fn)(const void *),const void *arg,size_t size)
{ (void)size;fn(arg); }
