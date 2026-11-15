#ifndef ZN_EXPORT_H
#define ZN_EXPORT_H

#if defined(ZN_BUILD_SHARED)
#  if defined(_WIN32) && defined(ZN_EXPORTS)
#    define ZN_API __declspec(dllexport)
#  elif defined(_WIN32)
#    define ZN_API __declspec(dllimport)
#  elif defined(__GNUC__)
#    define ZN_API __attribute__((visibility("default")))
#  else
#    define ZN_API
#  endif
#else
#  define ZN_API
#endif

#endif /* ZN_EXPORT_H */
