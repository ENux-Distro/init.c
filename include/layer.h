/*
 * layer.h — layer discovery and boot-time enablement
 *
 * Replaces the Bedrock stratum machinery. A layer is just a rootfs under
 * /enux/layer/<name>; enablement is a fork of /enux/libexec/layer-enable,
 * the same script the user-facing tooling uses.
 */

#ifndef ENUX_LAYER_H
#define ENUX_LAYER_H

#include "init.h"

/* Fill st->layers from the directories under /enux/layer (symlink aliases
 * skipped). Returns the layer count, or -1 on error. */
int scan_layers(InitState *st);

/* Fork /enux/libexec/layer-enable for every scanned layer except the base,
 * all at once, and wait. Failures are warned, never fatal. */
void enable_layers(InitState *st);

#endif /* ENUX_LAYER_H */
