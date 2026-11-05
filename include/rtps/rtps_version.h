#ifndef RTPS_VERSION_H
#define RTPS_VERSION_H

/*
 * Version of the public rtps_* interface, independent of the RTPS protocol
 * version (which is 2.4) and of the engine implementation.
 *
 * Every public configuration or data structure starts with a uint32_t
 * struct_size. Callers fill it with sizeof(their struct); the library accepts
 * a size it knows and ignores trailing bytes, so structs can grow without
 * breaking existing binaries.
 */

#define RTPS_API_VERSION_MAJOR 1u
#define RTPS_API_VERSION_MINOR 1u
#define RTPS_API_VERSION ((RTPS_API_VERSION_MAJOR * 100u) + RTPS_API_VERSION_MINOR)

/*
 * Symbol visibility. The library is built statically today, so RTPS_API is
 * empty; it is the single place to add __declspec(dllexport) or
 * __attribute__((visibility("default"))) when a shared build is added.
 */
#ifndef RTPS_API
#define RTPS_API
#endif

#endif /* RTPS_VERSION_H */
