/*
 * Application-level lwIP overrides for Host/Linux builds.
 *
 * This file is injected at the HEAD of the include path for the cmake lwIP
 * library build (Host arch only) via LWIP_CMAKE_OPTIONS in component.mk.
 * It pulls in the Sming framework's lwipopts.h first with #include_next,
 * then overrides specific values to prevent TCP PCB pool exhaustion during
 * automated testing.
 */
#ifndef LWIPOPTS_HOST_H
#define LWIPOPTS_HOST_H

/* Pull in the Sming framework's default lwipopts.h */
#include_next <lwipopts.h>

/*
 * MEMP_NUM_TCP_PCB: number of simultaneously active TCP connections.
 *
 * The framework default (4) is too small for the host CI smoke tests:
 * each test creates 2 connections that enter TIME_WAIT (lasting TCP_MSL*2).
 * With 4 PCBs the pool is exhausted after 2 test cycles, causing SYN drops.
 * 32 gives enough headroom for the full test suite.
 */
#ifdef MEMP_NUM_TCP_PCB
#undef MEMP_NUM_TCP_PCB
#endif
#define MEMP_NUM_TCP_PCB 32

/*
 * TCP_MSL: Maximum Segment Lifetime in milliseconds.
 *
 * Halves the TIME_WAIT duration (default 2*MSL = 120 s with MSL=60 000 ms).
 * 5 000 ms gives a 10 s TIME_WAIT which is acceptable for local/CI test loops.
 */
#ifdef TCP_MSL
#undef TCP_MSL
#endif
#define TCP_MSL 5000

#endif /* LWIPOPTS_HOST_H */
