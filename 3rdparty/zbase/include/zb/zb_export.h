/**
 * @file zb_export.h
 * @brief zbase 导出宏。
 */
#ifndef ZB_EXPORT_H
#define ZB_EXPORT_H

#if defined(ZB_BUILD_SHARED)
#if defined(_WIN32) && defined(ZB_EXPORTS)
#define ZB_API __declspec(dllexport)
#elif defined(_WIN32)
#define ZB_API __declspec(dllimport)
#elif defined(__GNUC__)
#define ZB_API __attribute__((visibility("default")))
#else
#define ZB_API
#endif
#else
#define ZB_API
#endif

#endif /* ZB_EXPORT_H */
